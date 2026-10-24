// Front end: turns the register state into self-contained draw records.
#include "vgx_state.h"
#include "vgx_fesplit.h"
#include "vgx_recipe_seed.h"
#include "vgx_variant_set.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace vgx {

namespace {

CmdBuf g_buf;
uint64_t g_frame = 1;

// ---- shader registry (ids are dense; sources are immutable and never freed)
template <typename K> struct Registry {
    struct Entry { K key; uint16_t id; };
    std::unordered_multimap<uint64_t, Entry> map;
    std::vector<std::unique_ptr<ShaderSource>> sources;
    std::vector<K> keys; // by id
    static constexpr uint32_t kFrontSize = 1024;  // a power of two (the 64 there were did not hold a scene's 200-odd programs: the misses cost divisions)
    struct Front { uint64_t h = 0; uint16_t id = 0; bool valid = false; } front[kFrontSize];
};
Registry<FsKey> g_fsReg;
Registry<VsKey> g_vsReg;

// ---- shader warm-up list
// A shader is generated the first time a draw needs it, and the back end
// compiles it there and then: on the console's replacement GPU that is a
// pause of many milliseconds in the middle of play (53 vertex and 47 fragment
// shaders, 78 programs over one match). So every key and every program pair
// is appended to a file as it first appears, and a run that finds the file
// generates all of them, and has the pairs linked, in its first frame.
// File: ListHeader, then records {u32 type; payload}: 1 = VsKey, 2 = FsKey
// (in id order of the run that wrote them), 3 = {u32 vsId, u32 fsId},
// 4 = {u32 vsId, fsId, layoutMask, blendKey}: a variant of a program the
// console's shader patcher builds at first use (VariantCmd in vgx_cmd.h);
// the back end is asked for an empty draw with each (CMD_VARIANT).
struct ListHeader {
    char magic[8];
    uint32_t vsKeySize, fsKeySize, genVersion, reserved;
};
constexpr char kListMagic[8] = {'V', 'G', 'X', 'S', 'H', 'L', '0', '1'};
std::string g_shaderListPath;
FILE* g_shaderList = nullptr;   // open for appending once the list has been read
bool g_shaderListDone = false;  // read (or found absent / not configured)
bool g_shaderListReplay = false;
bool g_warmDeferred = false;    // SetShaderWarmupDeferred
std::array<uint8_t, 16u << 10> g_shaderListPending{};
size_t g_shaderListPendingSize = 0;
static_assert(sizeof(VsKey) + 4 <= 16u << 10 && sizeof(FsKey) + 4 <= 16u << 10);
// Where the list's replay puts its commands: CmdBuf::warm when the back end is to do them in slices, else with the frame's own.
inline std::vector<Cmd>& ReplayCmds() { return g_warmDeferred && g_shaderListReplay ? g_buf.warm : g_buf.cmds; }
std::unordered_set<uint32_t> g_progSeen;
VariantSet g_varSeen;
uint64_t g_lastVariant = ~0ull;         // of the last draw (most draws repeat it)
inline uint64_t VariantKey(uint32_t vsId, uint32_t fsId, uint32_t layoutMask, uint32_t blendKey) {
    return (static_cast<uint64_t>((vsId << 16) | fsId) << 32) | ((layoutMask & 0xFFFu) << 12) | (blendKey & 0xFFFu);
}
void EnsureShaderList();
bool ListQueue(uint32_t type, const void* p, size_t n);
bool ListFlush();
#if GXV_RECIPE_SEED_BATCH
std::string g_recipeSeedPath;
bool g_recipeMerged = false, g_recipeAppendOk = true;
// Warm identities may be seed-only; file IDs/seen sets follow accepted records in FIFO order.
std::vector<uint32_t> g_vsFileIds, g_fsFileIds;
uint32_t g_nextVsFileId = 0, g_nextFsFileId = 0;
std::unordered_set<uint32_t> g_filePrograms;
VariantSet g_fileVariants;

bool RecipeAppend(uint32_t type, const void* p, size_t n) {
    if (!g_recipeAppendOk || !g_shaderList || g_shaderListReplay) return false;
    return ListQueue(type, p, n);
}
template <typename K> bool PersistRecipeKey(uint16_t id, const K& key, bool fragment) {
    auto& ids = fragment ? g_fsFileIds : g_vsFileIds;
    uint32_t& next = fragment ? g_nextFsFileId : g_nextVsFileId;
    if (ids.size() <= id) ids.resize(static_cast<size_t>(id) + 1, recipe::kNoFileId);
    if (ids[id] != recipe::kNoFileId) return true;
    if (!RecipeAppend(fragment ? 2u : 1u, &key, sizeof key)) return false;
    ids[id] = next++;
    return true;
}
bool TryRecipeMerge();
#endif
void ListDisable() {
    g_shaderListPendingSize = 0;
#if GXV_RECIPE_SEED_BATCH
    g_recipeAppendOk = false;
#endif
    FILE* f = g_shaderList;
    g_shaderList = nullptr;
    if (f) std::fclose(f);
}
bool ListFlush() {
    if (!g_shaderListPendingSize) return true;
    const size_t n = g_shaderListPendingSize;
    g_shaderListPendingSize = 0;
    if (g_shaderList && std::fwrite(g_shaderListPending.data(), 1, n, g_shaderList) == n && std::fflush(g_shaderList) == 0) return true;
    ListDisable();
    VGX_ERR("shader recipe learning stopped after a write/flush failure\n");
    return false;
}
bool ListQueue(uint32_t type, const void* p, size_t n) {
    if (!g_shaderList || g_shaderListReplay) return false;
    if (n > g_shaderListPending.size() - sizeof type) { ListDisable(); return false; }
    // Capacity flushes end at a complete record, so any prefix still has valid dependencies.
    if (g_shaderListPendingSize > g_shaderListPending.size() - sizeof type - n && !ListFlush()) return false;
    std::memcpy(g_shaderListPending.data() + g_shaderListPendingSize, &type, sizeof type);
    g_shaderListPendingSize += sizeof type;
    std::memcpy(g_shaderListPending.data() + g_shaderListPendingSize, p, n);
    g_shaderListPendingSize += n;
    return true;
}
inline void ListAppend(uint32_t type, const void* p, size_t n) {
    ListQueue(type, p, n);
}

template <typename K, typename Gen>
uint16_t Lookup(Registry<K>& reg, const K& key, bool fragment, Gen gen) {
    if (!g_shaderListDone) EnsureShaderList();
    const uint64_t h = HashKey(&key, sizeof(K));
    // (in front of the hash table: the last id seen for each of 1024 hash slots; the table's
    // lookup costs divisions, and the same few keys come round all frame)
    typename Registry<K>::Front& fr = reg.front[(h ^ (h >> 32)) & (Registry<K>::kFrontSize - 1)];
    if (fr.valid && fr.h == h && SameStruct(reg.keys[fr.id], key)) return fr.id;
    auto range = reg.map.equal_range(h);
    for (auto it = range.first; it != range.second; ++it)
        if (std::memcmp(&it->second.key, &key, sizeof(K)) == 0) {
            fr.h = h; fr.id = it->second.id; fr.valid = true;
            return it->second.id;
        }
    auto src = std::make_unique<ShaderSource>();
    src->fragment = fragment;
    src->id = static_cast<uint16_t>(reg.sources.size());
    src->keyHash = h;
    gen(key, *src);
    g_buf.shaders.push_back(src.get());
    ReplayCmds().push_back(Cmd{CMD_SHADER, static_cast<uint32_t>(g_buf.shaders.size() - 1)});
    reg.map.emplace(h, typename Registry<K>::Entry{key, src->id});
    reg.keys.push_back(key);
    reg.sources.push_back(std::move(src));
    stats::ShaderCreated(fragment, g_frame);
    VGX_LOG("%s shader %u generated in frame %llu%s\n", fragment ? "fragment" : "vertex", static_cast<unsigned>(reg.sources.size() - 1),
            static_cast<unsigned long long>(g_frame), reg.sources.back()->usesPalette ? " (reads the matrix palette)" : "");
#if GXV_RECIPE_SEED_BATCH
    if (g_recipeMerged) {
        if (!g_shaderListReplay) PersistRecipeKey(reg.sources.back()->id, key, fragment);
    } else
#endif
    ListAppend(fragment ? 2u : 1u, &key, sizeof(K));
    fr.h = h; fr.id = static_cast<uint16_t>(reg.sources.size() - 1); fr.valid = true;
    return static_cast<uint16_t>(reg.sources.size() - 1);
}

template <typename K> bool ListWriteKeys(FILE* f, const Registry<K>& reg, uint32_t type) {
    std::vector<const K*> byId(reg.sources.size(), nullptr);
    for (const auto& kv : reg.map) byId[kv.second.id] = &kv.second.key;
    for (const K* k : byId) {
        if (std::fwrite(&type, 4, 1, f) != 1 || std::fwrite(k, sizeof(K), 1, f) != 1) return false;
    }
    return true;
}

#if GXV_RECIPE_SEED_BATCH
void ReplayRecipe(const recipe::List& list, bool persisted) {
    std::vector<uint16_t> vsIds, fsIds;
    for (const auto& rec : list.records) {
        if (rec.type == 1) {
            const uint16_t id = Lookup(g_vsReg, list.vs[rec.v[0]], false, GenerateVertexShader);
            vsIds.push_back(id);
            if (persisted) {
                if (g_vsFileIds.size() <= id) g_vsFileIds.resize(static_cast<size_t>(id) + 1, recipe::kNoFileId);
                g_vsFileIds[id] = g_nextVsFileId++;
            }
        } else if (rec.type == 2) {
            const uint16_t id = Lookup(g_fsReg, list.fs[rec.v[0]], true, GenerateFragmentShader);
            fsIds.push_back(id);
            if (persisted) {
                if (g_fsFileIds.size() <= id) g_fsFileIds.resize(static_cast<size_t>(id) + 1, recipe::kNoFileId);
                g_fsFileIds[id] = g_nextFsFileId++;
            }
        } else {
            const uint16_t vs = vsIds[rec.v[0]], fs = fsIds[rec.v[1]];
            const uint32_t pk = (static_cast<uint32_t>(vs) << 16) | fs;
            if (rec.type == 3) {
                if (g_progSeen.insert(pk).second) g_buf.cmds.push_back(Cmd{CMD_PROGRAM, pk});
                if (persisted) g_filePrograms.insert(pk);
            } else {
                const uint64_t vk = VariantKey(vs, fs, rec.v[2], rec.v[3]);
                if (g_varSeen.insert(vk).second) {
                    g_buf.variants.push_back(VariantCmd{vs, fs, static_cast<uint16_t>(rec.v[2]), static_cast<uint16_t>(rec.v[3])});
                    g_buf.cmds.push_back(Cmd{CMD_VARIANT, static_cast<uint32_t>(g_buf.variants.size() - 1)});
                }
                if (persisted) g_fileVariants.insert(vk);
            }
        }
    }
}

bool TryRecipeMerge() {
    if (!recipe::Eligible() || g_recipeSeedPath.empty() || !g_vsReg.keys.empty() || !g_fsReg.keys.empty()) return false;
    auto refused = [] {
        std::fprintf(stderr, "[vgx] recipe seed: refused; ordinary learning retained\n");
        return false;
    };
    recipe::List seed, user;
    std::vector<uint8_t> userBytes;
    bool absent = false;
    // Neither source may publish commands until both complete snapshots pass.
    if (!recipe::ReadSeed(g_recipeSeedPath.c_str(), seed) ||
        !recipe::Read(g_shaderListPath.c_str(), recipe::kMaxListSize, userBytes, &absent) ||
        (!absent && !recipe::Parse(userBytes, user))) {
        return refused();
    }
    if (absent) {
        FILE* f = std::fopen(g_shaderListPath.c_str(), "wb");
        if (!f) return refused();
        recipe::Header h{};
        std::memcpy(h.magic, kListMagic, 8);
        h.vsSize = sizeof(VsKey); h.fsSize = sizeof(FsKey); h.version = kShaderGenVersion;
        const bool written = std::fwrite(&h, sizeof h, 1, f) == 1;
        const bool closed = std::fclose(f) == 0;
        if (!written || !closed) return refused();
    }
    g_shaderList = std::fopen(g_shaderListPath.c_str(), "ab");
    if (!g_shaderList) return refused();
    g_recipeMerged = true;
    g_shaderListReplay = true;
    ReplayRecipe(user, true);
    ReplayRecipe(seed, false);
    g_shaderListReplay = false;
    std::fprintf(stderr, "[vgx] recipe seed: accepted; %u vertex, %u fragment, %u programs, %u variants (user bytes retained)\n",
            static_cast<unsigned>(g_vsReg.keys.size()), static_cast<unsigned>(g_fsReg.keys.size()),
            static_cast<unsigned>(g_progSeen.size()), static_cast<unsigned>(g_varSeen.size()));
    return true;
}

void PersistRecipeDraw(uint16_t vsId, uint16_t fsId, uint32_t layoutMask, uint32_t bk) {
    if (!g_recipeAppendOk) return;
    if (!PersistRecipeKey(vsId, g_vsReg.keys[vsId], false) || !PersistRecipeKey(fsId, g_fsReg.keys[fsId], true)) return;
    const uint32_t pk = (static_cast<uint32_t>(vsId) << 16) | fsId;
    const uint64_t vk = VariantKey(vsId, fsId, layoutMask, bk);
    const uint32_t vs = g_vsFileIds[vsId], fs = g_fsFileIds[fsId];
    if (g_filePrograms.count(pk) == 0) {
        const uint32_t v[2] = {vs, fs};
        if (!RecipeAppend(3u, v, sizeof v)) return;
        g_filePrograms.insert(pk);
    }
    if (g_fileVariants.count(vk) == 0) {
        const uint32_t v[4] = {vs, fs, layoutMask & 0xFFFu, bk};
        if (!RecipeAppend(4u, v, sizeof v)) return;
        g_fileVariants.insert(vk);
    }
    g_progSeen.insert(pk);
    g_varSeen.insert(vk);
    g_lastVariant = vk;
}
#endif

void EnsureShaderList() {
    g_shaderListDone = true;
    if (g_shaderListPath.empty()) {
        if (const char* e = std::getenv("GXV_SHADERLIST")) g_shaderListPath = e;
    }
    if (g_shaderListPath.empty()) return;
#if GXV_RECIPE_SEED_BATCH
    if (TryRecipeMerge()) return;
#endif
    bool rewrite = true; // the file is missing, from another generator, damaged or has duplicates
    uint32_t nVs = 0, nFs = 0, nProg = 0, nVar = 0;
    if (FILE* f = std::fopen(g_shaderListPath.c_str(), "rb")) {
        ListHeader hd;
        if (std::fread(&hd, sizeof hd, 1, f) == 1 && std::memcmp(hd.magic, kListMagic, 8) == 0 && hd.vsKeySize == sizeof(VsKey) &&
            hd.fsKeySize == sizeof(FsKey) && hd.genVersion == kShaderGenVersion) {
            rewrite = false;
            g_shaderListReplay = true;
            std::vector<uint16_t> vsIds, fsIds; // id in this run of the n-th key in the file
            uint32_t type;
            for (;;) {
                const size_t typeBytes = std::fread(&type, 1, sizeof type, f);
                if (typeBytes != sizeof type) {
                    if (typeBytes || std::ferror(f)) rewrite = true;
                    break;
                }
                if (type == 1) {
                    VsKey k;
                    if (std::fread(&k, sizeof k, 1, f) != 1) { rewrite = true; break; }
                    const uint16_t id = Lookup(g_vsReg, k, false, GenerateVertexShader);
                    if (id != vsIds.size()) rewrite = true;
                    vsIds.push_back(id);
                } else if (type == 2) {
                    FsKey k;
                    if (std::fread(&k, sizeof k, 1, f) != 1) { rewrite = true; break; }
                    const uint16_t id = Lookup(g_fsReg, k, true, GenerateFragmentShader);
                    if (id != fsIds.size()) rewrite = true;
                    fsIds.push_back(id);
                } else if (type == 3) {
                    uint32_t v[2];
                    if (std::fread(v, sizeof v, 1, f) != 1 || v[0] >= vsIds.size() || v[1] >= fsIds.size()) { rewrite = true; break; }
                    const uint32_t pk = (static_cast<uint32_t>(vsIds[v[0]]) << 16) | fsIds[v[1]];
                    if (g_progSeen.insert(pk).second) ReplayCmds().push_back(Cmd{CMD_PROGRAM, pk});
                    else rewrite = true;
                } else if (type == 4) {
                    uint32_t v[4];
                    if (std::fread(v, sizeof v, 1, f) != 1 || v[0] >= vsIds.size() || v[1] >= fsIds.size() || v[2] > 0xFFFu || v[3] > 0xFFFu) { rewrite = true; break; }
                    if (g_varSeen.insert(VariantKey(vsIds[v[0]], fsIds[v[1]], v[2], v[3])).second) {
                        g_buf.variants.push_back(VariantCmd{vsIds[v[0]], fsIds[v[1]], static_cast<uint16_t>(v[2]), static_cast<uint16_t>(v[3])});
                        ReplayCmds().push_back(Cmd{CMD_VARIANT, static_cast<uint32_t>(g_buf.variants.size() - 1)});
                        ++nVar;
                    } else {
                        rewrite = true;
                    }
                } else {
                    rewrite = true;
                    break;
                }
            }
            g_shaderListReplay = false;
            nVs = static_cast<uint32_t>(g_vsReg.sources.size());
            nFs = static_cast<uint32_t>(g_fsReg.sources.size());
            nProg = static_cast<uint32_t>(g_progSeen.size());
        }
        const bool readFailed = std::ferror(f) != 0;
        const bool closed = std::fclose(f) == 0;
        // A transient read error is not evidence that the existing file is corrupt.
        if (readFailed || !closed) return;
    } else if (errno != ENOENT) {
        return;
    }
    if (rewrite) {
        if (FILE* f = std::fopen(g_shaderListPath.c_str(), "wb")) {
            ListHeader hd{};
            std::memcpy(hd.magic, kListMagic, 8);
            hd.vsKeySize = sizeof(VsKey);
            hd.fsKeySize = sizeof(FsKey);
            hd.genVersion = kShaderGenVersion;
            bool written = std::fwrite(&hd, sizeof hd, 1, f) == 1;
            if (written) written = ListWriteKeys(f, g_vsReg, 1u);
            if (written) written = ListWriteKeys(f, g_fsReg, 2u);
            std::vector<uint32_t> progs(g_progSeen.begin(), g_progSeen.end());
            std::sort(progs.begin(), progs.end());
            for (uint32_t pk : progs) {
                if (!written) break;
                const uint32_t rec[3] = {3u, pk >> 16, pk & 0xFFFFu};
                written = std::fwrite(rec, sizeof rec, 1, f) == 1;
            }
            std::vector<uint64_t> vars(g_varSeen.begin(), g_varSeen.end());
            std::sort(vars.begin(), vars.end());
            for (uint64_t vk : vars) {
                if (!written) break;
                const uint32_t rec[5] = {4u, static_cast<uint32_t>(vk >> 48), static_cast<uint32_t>(vk >> 32) & 0xFFFFu, static_cast<uint32_t>(vk >> 12) & 0xFFFu,
                                         static_cast<uint32_t>(vk) & 0xFFFu};
                written = std::fwrite(rec, sizeof rec, 1, f) == 1;
            }
            const bool closed = std::fclose(f) == 0;
            if (!written || !closed) return;
        } else return;
    }
    g_shaderList = std::fopen(g_shaderListPath.c_str(), "ab");
    VGX_LOG("shader warm-up list %s: %u vertex shaders, %u fragment shaders, %u programs, %u program variants prepared%s%s\n", g_shaderListPath.c_str(), nVs, nFs, nProg, nVar,
            rewrite ? " (file written anew)" : "", g_shaderList ? "" : " (cannot append to it)");
}

// ---- cached resolution of the current state
struct Resolved {
    FsKey fs;
    VsKey vs;
    uint16_t fsId = 0, vsId = 0;
    RenderState rs;
    TexBind tex[8];
    uint8_t texMask = 0;
    bool valid = false;
    bool cullAll = false;
    bool soft = false; // this draw's vertices are transformed to eye space on the CPU
    // Register folding (see FoldRegs below): foldOk says the current keys allow it, fold that
    // this draw uses it; useFs is the fragment shader the draw is recorded with (fsId, or the
    // variant that reads TEV registers 0 and 1 from the vertex colours).
    uint64_t mtxRowsUsed = ~0ull; // rows of the position / texture matrix memory the matrix blocks were built from
    bool foldOk = false, fold = false;
    uint16_t fsIdFold = 0, useFs = 0;
    bool fsIdFoldValid = false;
    bool shortPalOk = false; // what the vertex key was built with
    uint32_t vcdMtxMask = 0;
    uint32_t vcdGen = 0xFFFFFFFFu; // State::vcdGen when ResolveState last ran
    uint32_t stateSerial = 0; // bumped whenever rs, tex or texMask are built again (a record made at this serial has the same ones)
    uint16_t texW[8] = {}, texH[8] = {};
    // What each texmap resolves to, kept while its binding is unchanged (State::texSlotDirty):
    // a draw that changes one texmap, or only the TEV setup, does not look the others up again.
    uint8_t slotResolved = 0;
    TexBind slotBind[8];
    uint16_t slotW[8] = {}, slotH[8] = {};
} cur;

uint32_t g_blockOff[UB_COUNT] = {kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock};
uint32_t g_blockLen[UB_COUNT] = {}; // floats of the block at g_blockOff
// The highest position-matrix row the vertices of the next EmitDraw / EmitStaticDraw use
// (their PNMTXIDX), 0xFF: not known. Decides between the short and the full matrix palette.
uint32_t g_pendingPosMtxMax = 0xFF;
bool g_blocksStale = true; // after a flush nothing is in the arena any more

size_t g_pendingVtxStart = 0;
int g_lastDraw = -1; // index of the draw that may still be merged into
uint32_t g_lastRecSerial = 0; // Resolved::stateSerial of the state that draw was recorded (or last merged) with

inline bool EfbHasAlpha() { return (g.bp[0x43] & 7) == 1; }

uint8_t SwapTable(int t) {
    const uint32_t rg = g.bp[0xF6 + 2 * t], ba = g.bp[0xF7 + 2 * t];
    return static_cast<uint8_t>((rg & 3) | (((rg >> 2) & 3) << 2) | ((ba & 3) << 4) | (((ba >> 2) & 3) << 6));
}

void BuildFsKey(FsKey& k) {
    std::memset(&k, 0, sizeof k);
    const uint32_t gm = g.bp[0x00];
    k.numStages = static_cast<uint8_t>(bits(gm, 10, 4) + 1);
    k.numTexGens = static_cast<uint8_t>(bits(gm, 0, 4));
    if (k.numTexGens > 8) k.numTexGens = 8;
    k.numIndStages = static_cast<uint8_t>(bits(gm, 16, 3));
    if (k.numIndStages > 4) k.numIndStages = 4;
    bool anyInd = false;
    for (int s = 0; s < k.numStages; ++s) {
        FsKey::Stage& st = k.stage[s];
        const uint32_t tevc = g.bp[0xC0 + 2 * s], teva = g.bp[0xC1 + 2 * s];
        const uint32_t tref = g.bp[0x28 + (s >> 1)] >> ((s & 1) * 12);
        const uint32_t ksel = g.bp[0xF6 + (s >> 1)];
        st.color = tevc & 0x00FFFFFFu;
        st.alpha = teva & 0x00FFFFF0u;
        bool usesTex = false, usesRas = false, usesKc = false, usesKa = false;
        for (int i = 0; i < 4; ++i) {
            const uint32_t c = bits(tevc, i * 4, 4);
            if (c == 8 || c == 9) usesTex = true;
            if (c == 10 || c == 11) usesRas = true;
            if (c == 14) usesKc = true;
            const uint32_t a = bits(teva, 4 + i * 3, 3);
            if (a == 4) usesTex = true;
            if (a == 5) usesRas = true;
            if (a == 6) usesKa = true;
        }
        st.rasChan = 7;
        st.rasSwap = st.texSwap = 0xE4;
        if (k.numIndStages) {
            const uint32_t ind = g.bp[0x10 + s] & 0x1FFFFFu;
            // A stage is "direct" when it has no matrix, no bias and no wrapping.
            if (bits(ind, 9, 4) != 0 || bits(ind, 4, 3) != 0 || bits(ind, 13, 6) != 0 || bits(ind, 20, 1) != 0 ||
                bits(ind, 7, 2) != 0) {
                st.ind = ind;
                anyInd = true;
            }
        }
        if (usesTex || st.ind) st.texCoord = static_cast<uint8_t>(bits(tref, 3, 3));
        if (usesTex && bits(tref, 6, 1)) {
            st.texEnable = 1;
            st.texMap = static_cast<uint8_t>(bits(tref, 0, 3));
            st.texSwap = SwapTable(bits(teva, 2, 2));
        }
        if (usesRas) {
            st.rasChan = static_cast<uint8_t>(bits(tref, 7, 3));
            st.rasSwap = SwapTable(bits(teva, 0, 2));
        }
        if (usesKc) st.kcsel = static_cast<uint8_t>(bits(ksel, (s & 1) ? 14 : 4, 5));
        if (usesKa) st.kasel = static_cast<uint8_t>(bits(ksel, (s & 1) ? 19 : 9, 5));
    }
    if (!anyInd) k.numIndStages = 0;
    for (int i = 0; i < k.numIndStages; ++i) {
        const uint32_t r = g.bp[0x27] >> (i * 6);
        k.ind[i].texMap = static_cast<uint8_t>(bits(r, 0, 3));
        k.ind[i].texCoord = static_cast<uint8_t>(bits(r, 3, 3));
    }
    const uint32_t ac = g.bp[0xF3];
    const uint32_t c0 = bits(ac, 16, 3), c1 = bits(ac, 19, 3), op = bits(ac, 22, 2);
    // Fold the cases that always pass.
    const bool pass = (op == 0 && c0 == 7 && c1 == 7) || (op == 1 && (c0 == 7 || c1 == 7)) || (op == 3 && c0 == 7 && c1 == 7) ||
                      (op == 2 && ((c0 == 7 && c1 == 0) || (c0 == 0 && c1 == 7)));
    if (!pass) {
        k.alphaTestOn = 1;
        k.alphaTest = static_cast<uint8_t>(c0 | (c1 << 3) | (op << 6));
    }
    const uint32_t fsel = bits(g.bp[0xF1], 21, 3);
    if (fsel) k.fog = static_cast<uint8_t>(fsel | (bits(g.bp[0xF1], 20, 1) << 3));
    const uint32_t zop = bits(g.bp[0xF5], 2, 2);
    if (zop == 1) {
        stats::Note("z-texture (GXSetZTexture GX_ZT_ADD) requested: NOT implemented");
    } // (GX_ZT_REPLACE: BuildRenderState)
    for (int i = 0; i < k.numTexGens; ++i)
        if (bits(g.xfReg[0x40 + i], 1, 1)) k.texProjMask |= static_cast<uint8_t>(1u << i);
    const uint32_t cm0 = g.bp[0x41];
    const bool blending = bits(cm0, 0, 1) || bits(cm0, 11, 1);
    if (bits(g.bp[0x42], 8, 1) && EfbHasAlpha() && bits(cm0, 4, 1)) {
        if (!blending) k.dstAlpha = 1;
        else stats::Note("GXSetDstAlpha together with blending: constant alpha NOT written (needs a second pass)");
    }
}

void BuildVsKey(VsKey& k, uint8_t numTexGens, bool shortPalOk) {
    std::memset(&k, 0, sizeof k);
    k.numTexGens = numTexGens;
    k.numChans = static_cast<uint8_t>(g.xfReg[0x09] & 3);
    if (k.numChans > 2) k.numChans = 2;
    k.perVtxPos = g.vcd[0] ? 1 : 0;
    for (int i = 0; i < 4; ++i) {
        const int chanIdx = i & 1; // 0,1 colour; 2,3 alpha
        if (chanIdx >= k.numChans) continue;
        const uint32_t r = g.xfReg[0x0E + i];
        VsKey::Chan& c = k.chan[i];
        c.matSrcVtx = static_cast<uint8_t>(bits(r, 0, 1));
        c.enable = static_cast<uint8_t>(bits(r, 1, 1));
        if (c.enable) {
            c.ambSrcVtx = static_cast<uint8_t>(bits(r, 6, 1));
            c.lightMask = static_cast<uint8_t>(bits(r, 2, 4) | (bits(r, 11, 4) << 4));
            if (c.lightMask) {
                c.diffuse = static_cast<uint8_t>(bits(r, 7, 2));
                if (c.diffuse > 2) c.diffuse = 2;
                c.attn = bits(r, 9, 1) ? (bits(r, 10, 1) ? 2 : 1) : 0;
            }
        }
    }
    const bool dual = (g.xfReg[0x12] & 1) != 0;
    for (int i = 0; i < numTexGens; ++i) {
        const uint32_t r = g.xfReg[0x40 + i], d = g.xfReg[0x50 + i];
        VsKey::TexGen& t = k.tg[i];
        t.type = static_cast<uint8_t>(bits(r, 4, 3));
        if (t.type > 3) t.type = 0;
        t.src = static_cast<uint8_t>(bits(r, 7, 5));
        if (t.type >= 2) { t.src = 2; }
        else {
            t.proj = static_cast<uint8_t>(bits(r, 1, 1));
            t.inputAbc1 = static_cast<uint8_t>(bits(r, 2, 1));
            t.perVtxMtx = g.vcd[1 + i] ? 1 : 0;
            if (t.type == 0 && dual) {
                const bool norm = bits(d, 8, 1) != 0;
                if (norm) t.post = 2;
                else if (bits(d, 0, 6) != 61) t.post = 1;
            }
            if (t.type == 1) stats::Note("emboss (bump) texgen requested: NOT implemented (treated as regular)");
            if (t.src == 3 || t.src == 4) stats::Note("binormal/tangent texgen source: NOT implemented");
        }
    }
    // The short palette holds position rows 0..29 and their normal rows: enough when the
    // vertices use no higher row and no texture matrix comes from the palette.
    if (k.perVtxPos && shortPalOk) {
        bool texFromPal = false;
        for (int i = 0; i < numTexGens; ++i) texFromPal |= k.tg[i].perVtxMtx != 0;
        if (!texFromPal) k.perVtxPos = 2;
    }
}

uint8_t MapSrcFactor(uint32_t f, bool hasAlpha) {
    switch (f) {
    case 0: return BF_ZERO;
    case 1: return BF_ONE;
    case 2: return BF_DST_COLOR;
    case 3: return BF_INV_DST_COLOR;
    case 4: return BF_SRC_ALPHA;
    case 5: return BF_INV_SRC_ALPHA;
    case 6: return hasAlpha ? BF_DST_ALPHA : BF_ONE;
    default: return hasAlpha ? BF_INV_DST_ALPHA : BF_ZERO;
    }
}
uint8_t MapDstFactor(uint32_t f, bool hasAlpha) {
    switch (f) {
    case 0: return BF_ZERO;
    case 1: return BF_ONE;
    case 2: return BF_SRC_COLOR;
    case 3: return BF_INV_SRC_COLOR;
    case 4: return BF_SRC_ALPHA;
    case 5: return BF_INV_SRC_ALPHA;
    case 6: return hasAlpha ? BF_DST_ALPHA : BF_ONE;
    default: return hasAlpha ? BF_INV_DST_ALPHA : BF_ZERO;
    }
}

void BuildRenderState(RenderState& rs) {
    rs = RenderState{};
    const uint32_t cm0 = g.bp[0x41], zm = g.bp[0x40], gm = g.bp[0x00];
    const bool hasAlpha = EfbHasAlpha();
    if (bits(cm0, 11, 1)) { // subtract: dst - src
        rs.blend = 1; rs.blendEq = BE_REVSUB; rs.blendSrc = BF_ONE; rs.blendDst = BF_ONE;
    } else if (bits(cm0, 0, 1)) {
        rs.blend = 1;
        rs.blendSrc = MapSrcFactor(bits(cm0, 8, 3), hasAlpha);
        rs.blendDst = MapDstFactor(bits(cm0, 5, 3), hasAlpha);
        if (rs.blendSrc == BF_ONE && rs.blendDst == BF_ZERO) rs.blend = 0;
    } else if (bits(cm0, 1, 1)) {
        // Logic ops do not exist on the target GPU API; approximate with blending
        // (same table Dolphin uses for back ends without logic op support).
        static const uint8_t lo[16][2] = {
            {BF_ZERO, BF_ZERO}, {BF_DST_COLOR, BF_ZERO}, {BF_ONE, BF_INV_SRC_COLOR}, {BF_ONE, BF_ZERO},
            {BF_DST_COLOR, BF_ONE}, {BF_ZERO, BF_ONE}, {BF_INV_DST_COLOR, BF_INV_SRC_COLOR}, {BF_INV_DST_COLOR, BF_ONE},
            {BF_INV_SRC_COLOR, BF_INV_DST_COLOR}, {BF_INV_SRC_COLOR, BF_SRC_COLOR}, {BF_INV_DST_COLOR, BF_INV_DST_COLOR},
            {BF_ONE, BF_INV_DST_ALPHA}, {BF_INV_SRC_COLOR, BF_INV_SRC_COLOR}, {BF_INV_SRC_COLOR, BF_ONE},
            {BF_INV_DST_COLOR, BF_INV_SRC_COLOR}, {BF_ONE, BF_ONE}};
        const uint32_t op = bits(cm0, 12, 4);
        if (op != 3) {
            rs.blend = 1; rs.blendSrc = lo[op][0]; rs.blendDst = lo[op][1];
            stats::Note("logic-op blend mode used (approximated with blend factors)");
        }
    }
    rs.colorWrite = static_cast<uint8_t>(bits(cm0, 3, 1));
    rs.alphaWrite = static_cast<uint8_t>(bits(cm0, 4, 1) && hasAlpha);
    if (bits(zm, 0, 1)) {
        rs.depthTest = 1;
        rs.depthFunc = static_cast<uint8_t>(bits(zm, 1, 3));
        rs.depthWrite = static_cast<uint8_t>(bits(zm, 4, 1));
    }
    switch (bits(gm, 14, 2)) {
    case 1: rs.cull = CULL_BACK; break;
    case 2: rs.cull = CULL_FRONT; break;
    case 3: rs.cull = CULL_ALL; break;
    default: rs.cull = CULL_NONE; break;
    }
    rs.lineWidth6 = static_cast<uint8_t>(bits(g.bp[0x22], 0, 8));
    // Scissor (BP 0x20/0x21) and its offset (0x59), which also shifts the viewport.
    const int xoff = static_cast<int>(bits(g.bp[0x59], 0, 10)) * 2, yoff = static_cast<int>(bits(g.bp[0x59], 10, 10)) * 2;
    const int y0 = static_cast<int>(bits(g.bp[0x20], 0, 11)) - yoff, x0 = static_cast<int>(bits(g.bp[0x20], 12, 11)) - xoff;
    const int y1 = static_cast<int>(bits(g.bp[0x21], 0, 11)) - yoff, x1 = static_cast<int>(bits(g.bp[0x21], 12, 11)) - xoff;
    rs.scX = static_cast<int16_t>(x0); rs.scY = static_cast<int16_t>(y0);
    rs.scW = static_cast<int16_t>(x1 - x0 + 1); rs.scH = static_cast<int16_t>(y1 - y0 + 1);
    const float sx = u2f(g.xfReg[0x1A]), sy = u2f(g.xfReg[0x1B]), sz = u2f(g.xfReg[0x1C]);
    const float ox = u2f(g.xfReg[0x1D]), oy = u2f(g.xfReg[0x1E]), oz = u2f(g.xfReg[0x1F]);
    rs.vpW = 2.0f * sx; rs.vpH = -2.0f * sy;
    rs.vpX = ox - sx - static_cast<float>(xoff);
    rs.vpY = oy + sy - static_cast<float>(yoff);
    rs.zFar = oz / 16777215.0f;
    rs.zNear = (oz - sz) / 16777215.0f;
    if (rs.zNear < 0) rs.zNear = 0;
    if (rs.zFar > 1) rs.zFar = 1;
    if (bits(g.bp[0xF5], 2, 2) == 2) {
        // GX_ZT_REPLACE with a texture of one value (how the game clears depth): see BpZTexConstant.
        float z = 1.0f;
        if (BpZTexConstant(z)) rs.zNear = rs.zFar = z;
        else stats::Note("z-texture (GX_ZT_REPLACE) with a texture that is not one constant: NOT implemented");
    }
}

// ---- uniform blocks
// Appends block b (n floats) to the frame's uniform arena unless the block the
// current draws refer to already holds exactly these values.
// (The builders take where the blocks go and the register state they read: the front-end split
// (vgx_fesplit.h) runs them on its finishing thread with a copy of the registers; `g` inside them
// is that state.)
void PushBlock(split::BlockCtx& c, int b, const float* data, uint32_t n) {
    UniArena& uni = *c.uni;
    if (!c.stale && c.blockOff[b] != kNoBlock && c.blockLen[b] == n && SameWords(uni.data() + c.blockOff[b], data, n)) return;
    const size_t o = uni.size();
    c.blockOff[b] = static_cast<uint32_t>(o);
    c.blockLen[b] = n;
    // (a resize without a fill and a word copy: the general range insert, and the library copy it makes, cost ~160
    // instructions a block, 700 blocks a picture in the Comet Observatory)
    uni.resize(o + n);
    CopyWords(uni.data() + o, data, n);
}
} // namespace
namespace split {
void BuildProjC(BlockCtx& c, const State& g) {
    float p[6];
    for (int i = 0; i < 6; ++i) p[i] = u2f(g.xfReg[0x20 + i]);
    float m[16] = {};
    // Column-major. Y is flipped (the EFB is rendered top row first, see NOTES)
    // and GX clip z in [-w, 0] is remapped to GL's [-w, w].
    if ((g.xfReg[0x26] & 1) == 0) { // perspective
        m[0] = p[0]; m[8] = p[1];
        m[5] = -p[2]; m[9] = -p[3];
        m[10] = 2.0f * p[4] - 1.0f; m[14] = 2.0f * p[5];
        m[11] = -1.0f;
    } else {
        m[0] = p[0]; m[12] = p[1];
        m[5] = -p[2]; m[13] = -p[3];
        m[10] = 2.0f * p[4]; m[14] = 2.0f * p[5] + 1.0f;
        m[15] = 1.0f;
    }
    PushBlock(c, UB_PROJ, m, 16);
}
// Returns the rows of the position / texture matrix memory the blocks depend on.
// (MtxReads below names the rows this reads: keep the two together.)
uint64_t BuildMtxC(BlockCtx& c, const State& g, const VsKey& vs, bool soft) {
    float mv[24] = {};
    const uint32_t pi = g.matIdxA & 0x3F;
    uint64_t rows = 0;
    auto use = [&rows](uint32_t first) {
        for (uint32_t r = first; r < first + 3; ++r) rows |= 1ull << (r < 64 ? r : 63);
    };
    if (!soft) use(pi);
    if (soft) {
        // Vertices arrive in eye space already (see SoftTransform).
        mv[0] = mv[5] = mv[10] = 1.0f;
        mv[12] = mv[17] = mv[22] = 1.0f;
    } else {
        for (int r = 0; r < 3; ++r) {
            const uint32_t row = pi + r < 64 ? pi + r : 63;
            std::memcpy(&mv[r * 4], g.xfPos[row], 16);
            const uint32_t nrow = (pi + r) & 31;
            std::memcpy(&mv[12 + r * 4], g.xfNrm[nrow], 12);
        }
    }
    PushBlock(c, UB_MV, mv, 24);
    // Texture matrices [0, 96) and post-transform matrices [96, 192), 12 floats per texgen.
    // Packed: only up to the last texgen, and the second half only when a texgen uses it.
    const bool pack = UniformPacking();
    bool anyPost = false;
    for (int i = 0; i < vs.numTexGens; ++i) anyPost |= vs.tg[i].type < 2 && vs.tg[i].post != 0;
    const uint32_t tmLen = !pack ? 192u : (anyPost ? 96u + vs.numTexGens * 12u : vs.numTexGens * 12u);
    float tm[192];
    if (!pack) std::memset(tm, 0, sizeof tm);
    else if (anyPost) std::memset(tm + vs.numTexGens * 12, 0, (96 - vs.numTexGens * 12) * sizeof(float));
    for (int i = 0; i < vs.numTexGens; ++i) {
        const uint32_t ti = i < 4 ? bits(g.matIdxA, 6 + 6 * i, 6) : bits(g.matIdxB, 6 * (i - 4), 6);
        const uint32_t po = g.xfReg[0x50 + i] & 0x3F;
        use(ti);
        for (int r = 0; r < 3; ++r) {
            std::memcpy(&tm[i * 12 + r * 4], g.xfPos[ti + r < 64 ? ti + r : 63], 16);
            if (!pack || anyPost) std::memcpy(&tm[96 + i * 12 + r * 4], g.xfPost[po + r < 64 ? po + r : 63], 16);
        }
    }
    if (tmLen) PushBlock(c, UB_TEXM, tm, tmLen);
    else c.blockOff[UB_TEXM] = kNoBlock;
    bool pal = vs.perVtxPos != 0;
    for (int i = 0; i < vs.numTexGens; ++i) pal |= vs.tg[i].perVtxMtx != 0;
    if (pal) {
        float pb[384];
        if (vs.perVtxPos == 2) {
            // Short palette: position rows 0..29, then normal rows 0..29.
            std::memcpy(pb, g.xfPos, 30 * 4 * sizeof(float));
            for (int r = 0; r < 30; ++r) {
                pb[120 + r * 4] = g.xfNrm[r][0]; pb[121 + r * 4] = g.xfNrm[r][1]; pb[122 + r * 4] = g.xfNrm[r][2]; pb[123 + r * 4] = 0;
            }
            PushBlock(c, UB_PAL, pb, kPalRowsShort * 4);
        } else {
            std::memcpy(pb, g.xfPos, 256 * 4);
            for (int r = 0; r < 32; ++r) {
                pb[256 + r * 4] = g.xfNrm[r][0]; pb[257 + r * 4] = g.xfNrm[r][1]; pb[258 + r * 4] = g.xfNrm[r][2]; pb[259 + r * 4] = 0;
            }
            PushBlock(c, UB_PAL, pb, kPalRowsFull * 4);
        }
        rows = ~0ull;
    }
    return rows;
}
void MtxReads(const State& g, const VsKey& vs, bool soft, uint64_t& pos, uint32_t& nrm, uint64_t& post, uint64_t& used) {
    auto three = [](uint32_t first) {
        uint64_t m = 0;
        for (uint32_t r = first; r < first + 3; ++r) m |= 1ull << (r < 64 ? r : 63);
        return m;
    };
    const uint32_t pi = g.matIdxA & 0x3F;
    pos = used = post = 0;
    nrm = 0;
    if (!soft) {
        pos = used = three(pi);
        for (uint32_t r = 0; r < 3; ++r) nrm |= 1u << ((pi + r) & 31);
    }
    const bool pack = UniformPacking();
    bool anyPost = false;
    for (int i = 0; i < vs.numTexGens; ++i) anyPost |= vs.tg[i].type < 2 && vs.tg[i].post != 0;
    for (int i = 0; i < vs.numTexGens; ++i) {
        const uint32_t ti = i < 4 ? bits(g.matIdxA, 6 + 6 * i, 6) : bits(g.matIdxB, 6 * (i - 4), 6);
        const uint32_t po = g.xfReg[0x50 + i] & 0x3F;
        used |= three(ti);
        pos |= three(ti);
        if (!pack || anyPost) post |= three(po);
    }
    bool pal = vs.perVtxPos != 0;
    for (int i = 0; i < vs.numTexGens; ++i) pal |= vs.tg[i].perVtxMtx != 0;
    if (pal) {
        if (vs.perVtxPos == 2) {
            pos |= (1ull << 30) - 1;
            nrm |= (1u << 30) - 1;
        } else {
            pos = ~0ull;
            nrm = ~0u;
        }
        used = ~0ull;
    }
}
} // namespace split
namespace {
inline void Rgba8ToFloat(uint32_t c, float* o) {
    o[0] = static_cast<float>((c >> 24) & 0xFF) / 255.0f; o[1] = static_cast<float>((c >> 16) & 0xFF) / 255.0f;
    o[2] = static_cast<float>((c >> 8) & 0xFF) / 255.0f; o[3] = static_cast<float>(c & 0xFF) / 255.0f;
}
// GXV_LIGHTNORM (default 1; 0 = the registers as loaded): a light's DIRECTION goes into the
// block as zero unless the vertex shader of the draw can produce a different result for a
// different direction. The game initialises the direction only of lights that need one; for
// the others the register holds whatever was in the stack frame of the function that built
// the GXLightObj, and that differs between builds of the game code (found by the integration
// worker: wt/integ/NOTES.md, 5 Oct 06:05; pointer-like words with the plain translation, NaN
// and infinity patterns in 558 of 8457 survey frames with the optimised one). Two reasons:
// (1) the generated shader computes `ca = max(0.0, dot(ld, dir))` and multiplies it by the
//     light's zero coefficients; a non-finite or very large `dir` makes that NaN (inf * 0)
//     on any GPU, and the colour of the vertex with it;
// (2) the frame hash (GXV_HASH) must not depend on dead stack bytes.
// What "can depend" means follows GenerateVertexShader (vgx_shadergen.cpp), which is the only
// reader of the block: the direction (u_lt[b + 1]) is read by channels with attn 2 (spot),
// where it only reaches the result through cosatt.y / cosatt.z, and with attn 1 (specular),
// where it reaches it through cosatt.y / .z and distatt.y / .z. Channels with attn 0 and
// lights outside every channel's mask do not read it. The block is built per vertex key
// (see ResolveState), so the decision may use the key.
bool LightNorm() {
    static const bool on = EnvInt("GXV_LIGHTNORM", 1) != 0;
    return on;
}
// GXV_LIGHTNORM=2: census, one line every 600 frames and at frame 1 (counts since start).
struct LightNormCensus {
    uint64_t blocks = 0, zeroed = 0, zeroedNonFinite = 0, zeroedUnread = 0, kept = 0, keptNonFinite = 0;
} g_lnc;
bool LightNormCensusOn() {
    static const bool on = diag::Int("GXV_LIGHTNORM", 1) == 2;
    return on;
}
inline bool NonFiniteDir(const uint32_t* L) {
    return (L[13] & 0x7F800000u) == 0x7F800000u || (L[14] & 0x7F800000u) == 0x7F800000u || (L[15] & 0x7F800000u) == 0x7F800000u;
}
} // namespace
void split::BuildLightC(BlockCtx& bc, const State& g, const VsKey& vs) {
    const uint64_t g_frame = bc.frame; // (census message)
    // amb 2 x vec4, mat 2 x vec4, then per light {pos, dir, col, cosatt, distatt}. Packed: only
    // the lights up to the last one the shader uses.
    const int nvec = UniformPacking() ? VsLightCount(vs) : 44;
    const int nLights = (nvec - 4) / 5;
    float lb[176];
    Rgba8ToFloat(g.xfReg[0x0A], &lb[0]); Rgba8ToFloat(g.xfReg[0x0B], &lb[4]);
    Rgba8ToFloat(g.xfReg[0x0C], &lb[8]); Rgba8ToFloat(g.xfReg[0x0D], &lb[12]);
    uint32_t spotUse = 0, specUse = 0, allUse = 0xFF; // bit l: some channel reads light l's direction that way
    if (LightNorm()) {
        allUse = 0;
        for (int i = 0; i < 4; ++i) {
            const VsKey::Chan& c = vs.chan[i];
            if ((i & 1) >= vs.numChans || !c.enable) continue;
            if (c.attn == 2) spotUse |= c.lightMask;
            else if (c.attn == 1) specUse |= c.lightMask;
        }
    }
    for (int i = 0; i < nLights; ++i) {
        const uint32_t* L = g.xfLight[i];
        float* o = &lb[16 + i * 20];
        for (int c = 0; c < 3; ++c) {
            o[c] = u2f(L[10 + c]);      // pos
            o[4 + c] = u2f(L[13 + c]);  // dir
            o[12 + c] = u2f(L[4 + c]);  // cos attenuation
            o[16 + c] = u2f(L[7 + c]);  // distance attenuation
        }
        o[3] = o[7] = o[15] = o[19] = 0.0f;
        Rgba8ToFloat(L[3], &o[8]);
        if (!(allUse & (1u << i))) {
            // (+0 and -0 both count as zero: bits without the sign)
            const bool cosConst = ((L[5] | L[6]) << 1) == 0, distConst = ((L[8] | L[9]) << 1) == 0;
            const bool live = ((spotUse & (1u << i)) && !cosConst) || ((specUse & (1u << i)) && !(cosConst && distConst));
            if (!live) o[4] = o[5] = o[6] = 0.0f;
            if (bc.census && LightNormCensusOn()) {
                const bool nz = ((L[13] | L[14] | L[15]) << 1) != 0, nf = NonFiniteDir(L);
                if (live) { ++g_lnc.kept; g_lnc.keptNonFinite += nf; }
                else if (nz) {
                    ++g_lnc.zeroed; g_lnc.zeroedNonFinite += nf; g_lnc.zeroedUnread += !((spotUse | specUse) & (1u << i));
                    static uint64_t lastLogged = ~0ull;
                    if (nf && ((spotUse | specUse) & (1u << i)) && lastLogged != g_frame) {
                        lastLogged = g_frame;
                        std::fprintf(stderr, "[vgx] frame %llu: light %d has a non-finite direction (%08x %08x %08x) that a shader reads and multiplies by zero\n",
                                     (unsigned long long)g_frame, i, L[13], L[14], L[15]);
                    }
                }
            }
        }
    }
    if (bc.census && LightNormCensusOn()) ++g_lnc.blocks;
    PushBlock(bc, UB_LIGHT, lb, static_cast<uint32_t>(nvec) * 4);
}
namespace {
inline float FogFloat(uint32_t r) { // sign(1) exp(8) mantissa(11) packed in the low 20 bits
    return u2f((bits(r, 19, 1) << 31) | (bits(r, 11, 8) << 23) | (bits(r, 0, 11) << 12));
}
} // namespace
// (reads of the registers: TEV colour / konst registers, BP 0x06-0x0E, 0x25-0x26, 0x42, 0xEE-0xF4; of the key: fog,
// numIndStages (the split sends exactly these: vgx_fesplit.cpp, Tev))
void split::BuildTevC(BlockCtx& bc, const State& g, const FsKey& fs, const uint16_t texW[8], const uint16_t texH[8], bool fold) {
    float tb[112];
    const uint32_t tevLen = static_cast<uint32_t>(FsTevCount(fs)) * 4;
    // (nothing beyond tevLen is written: fog and indirect values only with their keys. A word loop: tevLen is 28..112 floats,
    // and the library call for those few bytes cost more than the stores)
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(VGX_NO_NEON)
    for (uint32_t i = 0; i < tevLen; i += 4) vst1q_f32(tb + i, vdupq_n_f32(0.0f)); // (tevLen: a multiple of 4)
#else
    std::memset(tb, 0, tevLen * sizeof(float));
#endif
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 4; ++c) {
            // (folded: registers 0 and 1 travel in the vertices, and the block must not differ by them)
            tb[TEV_REG + i * 4 + c] = (fold && (i == 1 || i == 2)) ? 0.0f : static_cast<float>(g.tevReg[i][c]) / 255.0f;
            tb[TEV_KONST + i * 4 + c] = static_cast<float>(g.tevKonst[i][c] & 0xFF) / 255.0f;
        }
    tb[TEV_MISC + 0] = static_cast<float>(bits(g.bp[0xF3], 0, 8));
    tb[TEV_MISC + 1] = static_cast<float>(bits(g.bp[0xF3], 8, 8));
    tb[TEV_MISC + 2] = static_cast<float>(bits(g.bp[0x42], 0, 8)) / 255.0f;
    tb[TEV_MISC + 3] = static_cast<float>(g.bp[0xF4]) / 16777215.0f;
    if (fs.fog) {
        const float a = FogFloat(g.bp[0xEE]);
        const float bm = static_cast<float>(g.bp[0xEF]);
        const float sh = std::ldexp(1.0f, static_cast<int>(bits(g.bp[0xF0], 0, 5)));
        if (fs.fog & 8) { tb[TEV_FOG + 0] = a; tb[TEV_FOG + 1] = 0; }
        else { tb[TEV_FOG + 0] = a * sh; tb[TEV_FOG + 1] = bm * sh / 16777216.0f; }
        tb[TEV_FOG + 2] = FogFloat(g.bp[0xF1]);
        tb[TEV_FOGCOL + 0] = static_cast<float>(bits(g.bp[0xF2], 16, 8)) / 255.0f;
        tb[TEV_FOGCOL + 1] = static_cast<float>(bits(g.bp[0xF2], 8, 8)) / 255.0f;
        tb[TEV_FOGCOL + 2] = static_cast<float>(bits(g.bp[0xF2], 0, 8)) / 255.0f;
    }
    if (fs.numIndStages) {
        for (int m = 0; m < 3; ++m) {
            const uint32_t r0 = g.bp[0x06 + m * 3], r1 = g.bp[0x07 + m * 3], r2 = g.bp[0x08 + m * 3];
            const int sc = static_cast<int>(bits(r0, 22, 2) | (bits(r1, 22, 2) << 2) | (bits(r2, 22, 2) << 4)) - 17;
            auto s11 = [](uint32_t v) { return static_cast<float>(static_cast<int>(v << 21) >> 21) / 1024.0f; };
            float* o = &tb[TEV_INDMTX + m * 8];
            // Offsets are produced in texels of the regular texture coordinate (see shadergen).
            const float scale = std::ldexp(1.0f, sc);
            o[0] = s11(bits(r0, 0, 11)) * scale; o[1] = s11(bits(r1, 0, 11)) * scale; o[2] = s11(bits(r2, 0, 11)) * scale; o[3] = scale;
            o[4] = s11(bits(r0, 11, 11)) * scale; o[5] = s11(bits(r1, 11, 11)) * scale; o[6] = s11(bits(r2, 11, 11)) * scale;
        }
        for (int i = 0; i < fs.numIndStages; ++i) {
            const uint32_t r = g.bp[0x25 + (i >> 1)] >> ((i & 1) * 8);
            float* o = &tb[TEV_INDSCALE + i * 4];
            o[0] = std::ldexp(1.0f, -static_cast<int>(bits(r, 0, 4)));
            o[1] = std::ldexp(1.0f, -static_cast<int>(bits(r, 4, 4)));
        }
        // Texture sizes per texmap: indirect offsets are in texels.
        for (int m = 0; m < 8; ++m) {
            tb[TEV_TEXSIZE + m * 2] = texW[m] ? static_cast<float>(texW[m]) : 1.0f;
            tb[TEV_TEXSIZE + m * 2 + 1] = texH[m] ? static_cast<float>(texH[m]) : 1.0f;
        }
    }
    PushBlock(bc, UB_TEV, tb, tevLen);
}

// Small meshes can be transformed to eye space on the CPU so that consecutive
// draws that differ only in their model-view matrix (grass blades, crowd
// sprites, particles: hundreds per frame in this game) share one uniform set
// and merge into a single draw call. GXV_SOFTXFORM=<max verts>, 0 = off.
uint32_t SoftXformLimit() {
    static const uint32_t n = static_cast<uint32_t>(EnvInt("GXV_SOFTXFORM", 128));
    return n;
}
namespace {
bool SoftEligible(const VsKey& vs) {
    if (vs.perVtxPos) return false;
    for (int i = 0; i < vs.numTexGens; ++i)
        if (vs.tg[i].perVtxMtx || (vs.tg[i].type < 2 && vs.tg[i].src < 2)) return false; // needs model-space pos / normal
    return true;
}
// The plain loop (the reference for the NEON one below).
static void SoftTransformScalar(uint8_t* v, uint32_t count, const VtxLayout& L, const float* m0, const float* m1, const float* m2, const float* n0,
                                const float* n1, const float* n2, bool nrm) {
    for (uint32_t i = 0; i < count; ++i, v += L.stride) {
        float* p = reinterpret_cast<float*>(v);
        const float x = p[0], y = p[1], z = p[2];
        p[0] = m0[0] * x + m0[1] * y + m0[2] * z + m0[3];
        p[1] = m1[0] * x + m1[1] * y + m1[2] * z + m1[3];
        p[2] = m2[0] * x + m2[1] * y + m2[2] * z + m2[3];
        if (nrm) {
            float n[3];
            std::memcpy(n, v + L.offNrm, 12);
            const float o[3] = {n0[0] * n[0] + n0[1] * n[1] + n0[2] * n[2], n1[0] * n[0] + n1[1] * n[1] + n1[2] * n[2],
                                n2[0] * n[0] + n2[1] * n[1] + n2[2] * n[2]};
            std::memcpy(v + L.offNrm, o, 12);
        }
    }
}
} // namespace
// The position matrix rows (m: 12 floats) and the normal matrix rows (n: 9 floats) the soft transform uses.
static inline const float* SoftPosRows() { const uint32_t pi = g.matIdxA & 0x3F; return g.xfPos[pi < 62 ? pi : 61]; }
static inline const float* SoftNrmRows() { const uint32_t pi = g.matIdxA & 0x3F; return g.xfNrm[(pi & 31) < 30 ? (pi & 31) : 29]; }
void split::SoftTransformWith(uint8_t* v, uint32_t count, const VtxLayout& L, const float* m, const float* n) {
    const float* m0 = m;
    const float* m1 = m0 + 4;
    const float* m2 = m0 + 8;
    const float* n0 = n;
    const float* n1 = n0 + 3;
    const float* n2 = n0 + 6;
    const bool nrm = (L.mask & VL_NRM) != 0;
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(VGX_NO_NEON)
    // The same sums in the same order (rows times the vertex, left to right), four lanes at a time: a
    // vertex costs about a dozen instructions instead of forty. The matrix goes in by columns, built
    // once per call. (Multiply-accumulate on this core rounds the product first, like the scalar code.)
    // GXV_SOFTNEON=0 runs the scalar loop below instead (to compare instruction counts and frame hashes).
    // GXV_SOFTNEON=2 also runs the scalar loop on a copy and compares the results bit for bit (logged).
    static const int neonMode = EnvInt("GXV_SOFTNEON", 1);
    if (neonMode != 0) {
        uint8_t* const v0 = v;
        static std::vector<uint8_t> ref;
        if (diag::kEnabled && neonMode == 2) ref.assign(v, v + static_cast<size_t>(count) * L.stride);
        const float32x4_t r0 = vld1q_f32(m0), r1 = vld1q_f32(m1), r2 = vld1q_f32(m2), r3 = vdupq_n_f32(0.0f);
        const float32x4x2_t t01 = vtrnq_f32(r0, r1), t23 = vtrnq_f32(r2, r3);
        const float32x4_t c0 = vcombine_f32(vget_low_f32(t01.val[0]), vget_low_f32(t23.val[0]));
        const float32x4_t c1 = vcombine_f32(vget_low_f32(t01.val[1]), vget_low_f32(t23.val[1]));
        const float32x4_t c2 = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t23.val[0]));
        const float32x4_t c3 = vcombine_f32(vget_high_f32(t01.val[1]), vget_high_f32(t23.val[1]));
        float32x4_t d0 = vdupq_n_f32(0.0f), d1 = d0, d2 = d0;
        if (nrm) {
            const float cols[12] = {n0[0], n1[0], n2[0], 0.0f, n0[1], n1[1], n2[1], 0.0f, n0[2], n1[2], n2[2], 0.0f};
            d0 = vld1q_f32(cols); d1 = vld1q_f32(cols + 4); d2 = vld1q_f32(cols + 8);
        }
        for (uint32_t i = 0; i < count; ++i, v += L.stride) {
            float* p = reinterpret_cast<float*>(v);
            const float32x4_t in = vld1q_f32(p);
            const float32x2_t xy = vget_low_f32(in), zw = vget_high_f32(in);
            float32x4_t r = vmulq_lane_f32(c0, xy, 0);
            r = vmlaq_lane_f32(r, c1, xy, 1);
            r = vmlaq_lane_f32(r, c2, zw, 0);
            r = vaddq_f32(r, c3);
            vst1_f32(p, vget_low_f32(r));
            vst1q_lane_f32(p + 2, r, 2); // (p[3] stays as it was)
            if (nrm) {
                float* q = reinterpret_cast<float*>(v + L.offNrm);
                const float32x2_t nxy = vld1_f32(q), nz = vld1_dup_f32(q + 2);
                float32x4_t o = vmulq_lane_f32(d0, nxy, 0);
                o = vmlaq_lane_f32(o, d1, nxy, 1);
                o = vmlaq_lane_f32(o, d2, nz, 0);
                vst1_f32(q, vget_low_f32(o));
                vst1q_lane_f32(q + 2, o, 2);
            }
        }
        if (diag::kEnabled && neonMode == 2) {
            static uint64_t calls = 0, bad = 0, verts = 0;
            SoftTransformScalar(ref.data(), count, L, m0, m1, m2, n0, n1, n2, nrm);
            ++calls;
            verts += count;
            if (std::memcmp(ref.data(), v0, ref.size()) != 0) {
                ++bad;
                VGX_LOG_N(8, "softneon: MISMATCH in call %llu (%u vertices, stride %u, normals %d)\n", static_cast<unsigned long long>(calls), count,
                          static_cast<unsigned>(L.stride), nrm ? 1 : 0);
            }
            if ((calls & 0x3FFFu) == 0)
                VGX_LOG_N(200, "softneon: %llu calls, %llu vertices compared, %llu mismatching calls\n", static_cast<unsigned long long>(calls),
                        static_cast<unsigned long long>(verts), static_cast<unsigned long long>(bad));
        }
        return;
    }
#endif
    SoftTransformScalar(v, count, L, m0, m1, m2, n0, n1, n2, nrm);
}
namespace {
inline void SoftTransform(uint8_t* v, uint32_t count, const VtxLayout& L) { split::SoftTransformWith(v, count, L, SoftPosRows(), SoftNrmRows()); }

// Register folding. The game's particles are drawn one quad at a time, each with its own
// position matrix and its own TEV register 0 / 1 (the particle's colour and fading alpha).
// The matrix is absorbed by the soft transform; the registers are uniforms, so every particle
// was a draw call of its own (1,400 of them on the title screen). When a small draw's shaders
// do not use the vertex colours for anything (no colour channel, no stage reads the
// rasterised colour, no texture coordinate is generated from a colour), the two registers
// are written into the vertices' colour slots instead and the fragment shader variant
// FsKey::regFromVtx starts its registers from the interpolated colours (constant across the
// primitive, since all its vertices carry the same value). GXV_FOLDREGS=0 turns it off.
bool FoldRegsEnabled() {
    static const bool on = EnvInt("GXV_FOLDREGS", 1) != 0;
    return on;
}
bool FoldKeysOk(const FsKey& fs, const VsKey& vs) {
    if (vs.numChans != 0) return false; // (the vertex shader then passes a_col0 / a_col1 through as they are)
    for (int i = 0; i < vs.numTexGens; ++i)
        if (vs.tg[i].type >= 2) return false;
    bool reads = false;
    for (int s = 0; s < fs.numStages; ++s) {
        const FsKey::Stage& st = fs.stage[s];
        if (st.rasChan != 7) return false;
        for (int i = 0; i < 4; ++i) {
            const uint32_t c = bits(st.color, i * 4, 4);
            if (c >= 2 && c <= 5) reads = true;
            const uint32_t a = bits(st.alpha, 4 + i * 3, 3);
            if (a == 1 || a == 2) reads = true;
        }
    }
    return reads;
}
inline bool FoldValuesOk() {
    uint32_t any = 0;
    for (int c = 0; c < 4; ++c) any |= static_cast<uint32_t>(g.tevReg[1][c]) | static_cast<uint32_t>(g.tevReg[2][c]);
    return (any & ~0xFFu) == 0; // 0..255 each (the registers are signed 11-bit)
}

// The uniform blocks of the current state: built here, or (front-end split, vgx_fesplit.h) sent to the finishing thread.
inline split::BlockCtx MainBlockCtx() { return split::BlockCtx{&g_buf.uni, g_blockOff, g_blockLen, g_blocksStale, true, g_frame}; }
inline void BlkProj() {
    if (split::g_on) { split::Proj(g_blocksStale); return; }
    split::BlockCtx c = MainBlockCtx();
    split::BuildProjC(c, g);
}
inline uint64_t BlkMtx(const VsKey& vs, bool soft) {
    if (split::g_on) return split::Mtx(cur.vsId, vs, soft, g_blocksStale);
    split::BlockCtx c = MainBlockCtx();
    return split::BuildMtxC(c, g, vs, soft);
}
inline void BlkLight(const VsKey& vs) {
    if (split::g_on) { split::Light(cur.vsId, vs, g_blocksStale); return; }
    split::BlockCtx c = MainBlockCtx();
    split::BuildLightC(c, g, vs);
}
inline void BlkTev(const FsKey& fs, const uint16_t texW[8], const uint16_t texH[8], bool fold) {
    if (split::g_on) { split::Tev(cur.fsId, fs, texW, texH, fold, g_blocksStale); return; }
    split::BlockCtx c = MainBlockCtx();
    split::BuildTevC(c, g, fs, texW, texH, fold);
}
// (the builders as they were called before the split: into this frame's arena, from the current registers)
[[maybe_unused]] void BuildLight(const VsKey& vs) { split::BlockCtx c = MainBlockCtx(); split::BuildLightC(c, g, vs); }
[[maybe_unused]] uint64_t BuildMtx(const VsKey& vs, bool soft) { split::BlockCtx c = MainBlockCtx(); return split::BuildMtxC(c, g, vs, soft); }

static void ResolveStateFull(uint32_t vcdMtxMask, uint32_t vertexCount, bool shortPalOk);
// Most draws follow one that left everything the keys and blocks are built from as it was (only a particle's vertices, position
// matrix and colours changed): then there is nothing to rebuild, and the only questions are whether the draw is transformed on the
// CPU and folds the registers as the last one did. GXV_RESOLVEFAST=0 always takes the full path, 2 takes both and compares what they left.
// (GXV_RSCACHE=0: this and ResolveStateFull are the path; otherwise ResolveStateCached below.)
void ResolveStateLegacy(uint32_t vcdMtxMask, uint32_t vertexCount) {
    // The short matrix palette is enough when the draw's vertices stay within its rows.
    const bool shortPalOk = UniformPacking() && g_pendingPosMtxMax <= static_cast<uint32_t>(kPalShortMaxRow);
    g_pendingPosMtxMax = 0xFF;
    static const int fastMode = static_cast<int>(EnvInt("GXV_RESOLVEFAST", 1));
    if (fastMode != 0 && cur.valid && !g_blocksStale && !g.DirtyAny() && vcdMtxMask == cur.vcdMtxMask &&
        !((vcdMtxMask & 1u) && shortPalOk != cur.shortPalOk) && (g.xfPosRowsDirty & cur.mtxRowsUsed) == 0) {
        const bool soft = vertexCount <= SoftXformLimit() && SoftEligible(cur.vs);
        if (soft == cur.soft) {
            const bool fold = soft && cur.foldOk && FoldValuesOk();
            if (fold == cur.fold && (!fold || cur.fsIdFoldValid) && (g.tevColDirty & (fold ? 0xF9u : 0xFFu)) == 0) {
                if (diag::kEnabled && fastMode == 2) {
                    static std::vector<uint8_t> before;
                    static uint64_t checked = 0, bad = 0;
                    const uint8_t* c = reinterpret_cast<const uint8_t*>(&cur);
                    before.assign(c, c + sizeof cur);
                    uint32_t offs[UB_COUNT];
                    std::memcpy(offs, g_blockOff, sizeof offs);
                    const size_t uni = g_buf.uni.size();
                    const uint32_t gen = g.vcdGen;
                    ResolveStateFull(vcdMtxMask, vertexCount, shortPalOk);
                    // (vcdGen is the one thing the full path updates)
                    cur.vcdGen = reinterpret_cast<const Resolved*>(before.data())->vcdGen;
                    ++checked;
                    if (std::memcmp(before.data(), &cur, sizeof cur) != 0 || std::memcmp(offs, g_blockOff, sizeof offs) != 0 || uni != g_buf.uni.size()) {
                        ++bad;
                        VGX_LOG_N(16, "resolve: MISMATCH (the fast path would have kept a state the full path changed)\n");
                    }
                    cur.vcdGen = gen;
                    if ((checked & 0xFFFFF) == 0) VGX_LOG_N(200, "resolve: %llu fast-path draws checked, %llu mismatching\n", static_cast<unsigned long long>(checked), static_cast<unsigned long long>(bad));
                    return;
                }
                g.xfPosRowsDirty = 0;
                g.tevColDirty = 0;
                cur.vcdGen = g.vcdGen;
                return;
            }
        }
    }
    ResolveStateFull(vcdMtxMask, vertexCount, shortPalOk);
}
static void ResolveStateFull(uint32_t vcdMtxMask, uint32_t vertexCount, bool shortPalOk) {
    // (see MarkBP / MarkXFReg in vgx_state.cpp for what sets which flag)
    const bool keysDirty = !cur.valid || g.bpKeyDirty || g.xfKeyDirty || g.xfRegDirty || vcdMtxMask != cur.vcdMtxMask ||
                           ((vcdMtxMask & 1u) && shortPalOk != cur.shortPalOk);
    const bool texDirty = keysDirty || g.texDirty;
    // (a key that was built again and came out the same leaves the blocks that only depend
    // on the key alone)
    bool fsChanged = false, vsChanged = false;
    const bool vcdChanged = !cur.valid || vcdMtxMask != cur.vcdMtxMask;
    if (keysDirty) {
        FsKey fs;
        BuildFsKey(fs);
        VsKey vs;
        BuildVsKey(vs, fs.numTexGens, shortPalOk);
        cur.shortPalOk = shortPalOk;
        fsChanged = !cur.valid || !SameStruct(fs, cur.fs);
        vsChanged = !cur.valid || !SameStruct(vs, cur.vs);
        if (fsChanged) { cur.fs = fs; cur.fsId = Lookup(g_fsReg, fs, true, GenerateFragmentShader); cur.fsIdFoldValid = false; }
        if (vsChanged) { cur.vs = vs; cur.vsId = Lookup(g_vsReg, vs, false, GenerateVertexShader); }
        if (fsChanged || vsChanged) cur.foldOk = FoldRegsEnabled() && FoldKeysOk(cur.fs, cur.vs);
        cur.vcdMtxMask = vcdMtxMask;
    }
    if (keysDirty || g.xfViewDirty || g.bpRsDirty) {
        BuildRenderState(cur.rs);
        cur.cullAll = cur.rs.cull == CULL_ALL;
        ++cur.stateSerial;
    }
    if (texDirty) {
        uint8_t mask = 0;
        for (int s = 0; s < cur.fs.numStages; ++s)
            if (cur.fs.stage[s].texEnable) mask |= static_cast<uint8_t>(1u << cur.fs.stage[s].texMap);
        for (int i = 0; i < cur.fs.numIndStages; ++i) mask |= static_cast<uint8_t>(1u << cur.fs.ind[i].texMap);
        cur.texMask = mask;
        ++cur.stateSerial;
        static const bool perSlot = EnvInt("GXV_TEXSLOTS", 1) != 0; // 0: look every used texmap up again (the old behaviour)
        cur.slotResolved &= static_cast<uint8_t>(~g.texSlotDirty);
        if (!perSlot) cur.slotResolved = 0;
        g.texSlotDirty = 0;
        for (int m = 0; m < 8; ++m) {
            const uint8_t bit = static_cast<uint8_t>(1u << m);
            if (!(mask & bit)) {
                cur.tex[m] = TexBind{};
                cur.texW[m] = cur.texH[m] = 0;
                continue;
            }
            if (!(cur.slotResolved & bit)) {
                cur.slotW[m] = cur.slotH[m] = 0;
                ResolveTexture(m, cur.slotBind[m], cur.slotW[m], cur.slotH[m]);
                cur.slotResolved |= bit;
            }
            cur.tex[m] = cur.slotBind[m];
            cur.texW[m] = cur.slotW[m];
            cur.texH[m] = cur.slotH[m];
        }
    }
    const bool soft = vertexCount <= SoftXformLimit() && SoftEligible(cur.vs);
    if (g_blocksStale || g.xfProjDirty || g.xfRegDirty) BlkProj();
    if (g_blocksStale || g.xfRegDirty || g.xfMtxDirty || (g.xfPosRowsDirty & cur.mtxRowsUsed) != 0 || vsChanged || vcdChanged || soft != cur.soft)
        cur.mtxRowsUsed = BlkMtx(cur.vs, soft);
    cur.soft = soft;
    // (the light block depends on the vertex key through its length when packed and through
    // the directions it carries when normalised)
    if (g_blocksStale || g.xfRegDirty || g.xfLightDirty || g.xfColorDirty || (vsChanged && (UniformPacking() || LightNorm()))) BlkLight(cur.vs);
    const bool fold = soft && cur.foldOk && FoldValuesOk();
    if (fold && !cur.fsIdFoldValid) {
        FsKey fk = cur.fs;
        fk.regFromVtx = 3;
        cur.fsIdFold = Lookup(g_fsReg, fk, true, GenerateFragmentShader);
        cur.fsIdFoldValid = true;
    }
    // (folded: registers 0 and 1 are not in the block, so a particle's new colour leaves it alone)
    if (g_blocksStale || g.bpDirty || (g.tevColDirty & (fold ? 0xF9u : 0xFFu)) != 0 || fsChanged || fold != cur.fold || (texDirty && cur.fs.numIndStages))
        BlkTev(cur.fs, cur.texW, cur.texH, fold);
    cur.fold = fold;
    cur.useFs = fold ? cur.fsIdFold : cur.fsId;
    g_blocksStale = false;
    g.bpDirty = g.bpKeyDirty = g.bpRsDirty = g.xfRegDirty = g.xfKeyDirty = g.xfViewDirty = g.xfProjDirty = g.xfColorDirty = g.xfMtxDirty = g.xfLightDirty = g.texDirty = false;
    g.xfPosRowsDirty = 0;
    g.tevColDirty = 0;
    cur.vcdGen = g.vcdGen;
    cur.valid = true;
}

// ==================================================================== resolution cache (GXV_RSCACHE)
// The game draws ~1,100 packets a picture (Comet Observatory), and switches material for ~400 of them: each switch replayed the
// material's registers and then ResolveStateFull built the fragment and vertex keys again (memset, a walk over the stages,
// compare with the current ones, hash 400 bytes, look the shader up), rebuilt the render state, the texture bindings and every
// uniform block whose dirty flag the replay had raised, including the ones raised only because an unrelated register changed
// (XF 0x1008, written by every shape's vertex-format list, sets the catch-all xfRegDirty: all blocks and both keys again).
// Materials repeat: the keys a picture needs are a few dozen, and the same register contents come round every picture.
//   * Shader keys: looked up by State::keyHash (vgx_state.cpp: a hash of exactly the registers BuildFsKey / BuildVsKey read,
//     kept up to date by the register writers) with the vertex-matrix mask and the short-palette flag, in a direct-mapped table
//     of 1,024 entries that holds the shader ids (the keys themselves are the registry's). A changed key is copied from the
//     registry; an unchanged hash means unchanged keys, whatever the flags say.
//   * Everything else is rebuilt on what it is really built from: the render state on the key registers, the viewport and
//     its own registers; the texture bindings when the fragment key or a binding changed; the blocks on their own flags, not
//     on xfRegDirty (no Build function reads the XF registers that flag stands for).
// The result is the same state, block for block (an untouched input rebuilds an identical block, which PushBlock never
// appends): GXV_RSCACHE=2 runs the old path after the cached one for every draw and compares the resolved state, the block
// offsets and the uniform words appended (logged as "rscache: MISMATCH"). 0: the old path only.
// (The hash is 64 bits; two different key-register contents with the same hash would draw with the wrong shader. The odds
// for the few thousand contents a session sees are about 1e-12; the verify mode would show it.)
struct KeyMemo {
    uint64_t h = 0;          // State::keyHash
    uint32_t vcdMtxMask = 0;
    uint16_t fsId = 0, vsId = 0, fsIdFold = 0;
    uint8_t spo = 0;         // short palette (only with a per-vertex position matrix)
    uint8_t valid = 0, fsIdFoldValid = 0;
};
constexpr uint32_t kKeyMemoSize = 1024;
KeyMemo g_keyMemo[kKeyMemoSize];
// The memo key of the keys in cur (valid: cur's keys were set by the cached path, or confirmed by the verify mode).
struct CurKey {
    uint64_t h = 0;
    uint32_t mask = 0;
    uint32_t slot = 0;
    uint8_t spo = 0;
    bool valid = false;
} g_curKey;
uint64_t g_keyMemoHits = 0, g_keyMemoMisses = 0;
uint64_t g_rsChecked = 0, g_rsBad = 0; // (GXV_RSCACHE=2)

inline uint32_t KeyMemoSlot(uint64_t h, uint32_t mask, uint8_t spo) {
    const uint32_t x = static_cast<uint32_t>(h) ^ static_cast<uint32_t>(h >> 32) ^ (mask * 0x9E3779B1u) ^ (spo ? 0x7F4A7C15u : 0u);
    return (x ^ (x >> 15)) & (kKeyMemoSize - 1);
}
// The legacy path's notion of "the keys may have changed" (it also decides when it records the short-palette flag, and when
// it rebuilds a render state whose depth comes from a Z texture: the cached path does those as it does).
inline bool LegacyKeysDirty(uint32_t vcdMtxMask, bool shortPalOk) {
    return !cur.valid || g.bpKeyDirty || g.xfKeyDirty || g.xfRegDirty || vcdMtxMask != cur.vcdMtxMask || ((vcdMtxMask & 1u) && shortPalOk != cur.shortPalOk);
}
inline bool ZTexReplace() { return bits(g.bp[0xF5], 2, 2) == 2; }
inline bool SameCurKey(uint32_t vcdMtxMask, uint8_t spo) {
    return g_curKey.valid && cur.valid && g_curKey.h == g.keyHash && g_curKey.mask == vcdMtxMask && g_curKey.spo == spo;
}

// The keys for the current registers into cur; fsChanged / vsChanged as ResolveStateFull has them.
void ResolveKeysCached(uint32_t vcdMtxMask, bool shortPalOk, uint8_t spo, bool& fsChanged, bool& vsChanged) {
    const uint64_t h = g.keyHash;
    const uint32_t slot = KeyMemoSlot(h, vcdMtxMask, spo);
    KeyMemo& m = g_keyMemo[slot];
    uint16_t fsId, vsId;
    if (m.valid && m.h == h && m.vcdMtxMask == vcdMtxMask && m.spo == spo) {
        fsId = m.fsId;
        vsId = m.vsId;
        ++g_keyMemoHits;
    } else {
        FsKey fs;
        BuildFsKey(fs);
        VsKey vs;
        BuildVsKey(vs, fs.numTexGens, shortPalOk);
        fsId = Lookup(g_fsReg, fs, true, GenerateFragmentShader);
        vsId = Lookup(g_vsReg, vs, false, GenerateVertexShader);
        m = KeyMemo{};
        m.h = h; m.vcdMtxMask = vcdMtxMask; m.spo = spo;
        m.fsId = fsId; m.vsId = vsId;
        m.valid = 1;
        ++g_keyMemoMisses;
    }
    fsChanged = !cur.valid || fsId != cur.fsId;
    vsChanged = !cur.valid || vsId != cur.vsId;
    // (word copies: a struct assignment of the 412-byte key is a library call)
    static_assert(sizeof(FsKey) % 4 == 0 && sizeof(VsKey) % 4 == 0, "keys are whole words");
    if (fsChanged) { CopyWords(&cur.fs, &g_fsReg.keys[fsId], sizeof(FsKey) / 4); cur.fsId = fsId; cur.fsIdFoldValid = false; }
    if (vsChanged) { CopyWords(&cur.vs, &g_vsReg.keys[vsId], sizeof(VsKey) / 4); cur.vsId = vsId; }
    if (fsChanged || vsChanged) cur.foldOk = FoldRegsEnabled() && FoldKeysOk(cur.fs, cur.vs);
    cur.vcdMtxMask = vcdMtxMask;
    g_curKey.h = h; g_curKey.mask = vcdMtxMask; g_curKey.spo = spo; g_curKey.slot = slot; g_curKey.valid = true;
}

// The register-folding variant of the current fragment shader (memoised with the keys).
uint16_t FoldShaderCached() {
    KeyMemo& m = g_keyMemo[g_curKey.slot];
    const bool mine = g_curKey.valid && m.valid && m.h == g_curKey.h && m.vcdMtxMask == g_curKey.mask && m.spo == g_curKey.spo && m.fsId == cur.fsId;
    if (mine && m.fsIdFoldValid) return m.fsIdFold;
    FsKey fk = cur.fs;
    fk.regFromVtx = 3;
    const uint16_t id = Lookup(g_fsReg, fk, true, GenerateFragmentShader);
    if (mine) { m.fsIdFold = id; m.fsIdFoldValid = 1; }
    return id;
}

inline void ClearResolveDirty() {
    g.bpDirty = g.bpKeyDirty = g.bpRsDirty = g.xfRegDirty = g.xfKeyDirty = g.xfViewDirty = g.xfProjDirty = g.xfColorDirty = g.xfMtxDirty = g.xfLightDirty = g.texDirty = false;
    g.xfPosRowsDirty = 0;
    g.tevColDirty = 0;
}

// SoftEligible(cur.vs), kept with the vertex key the cached path set (g_softOkVs: the id it was worked out for).
uint16_t g_softOkVs = 0xFFFF;
bool g_softOk = false;
inline bool SoftEligibleCur() {
    if (g_softOkVs != cur.vsId) { g_softOk = SoftEligible(cur.vs); g_softOkVs = cur.vsId; }
    return g_softOk;
}
void ResolveStateCached(uint32_t vcdMtxMask, uint32_t vertexCount) {
    static const bool packing = UniformPacking();
    const bool shortPalOk = packing && g_pendingPosMtxMax <= static_cast<uint32_t>(kPalShortMaxRow);
    g_pendingPosMtxMax = 0xFF;
    const uint8_t spo = ((vcdMtxMask & 1u) && shortPalOk) ? 1 : 0;
    const bool sameKeys = SameCurKey(vcdMtxMask, spo);
    // ---- nothing to rebuild: the keys are the same, and no flag that feeds a block or the render state is up
    // (bpKeyDirty / xfKeyDirty / xfRegDirty alone mean nothing once the key registers hash the same). The flags are
    // tested as three words: State's run of twelve bytes, without bytes 1, 3 and 4 (bpKeyDirty, xfRegDirty, xfKeyDirty).
    uint32_t fw[3];
    std::memcpy(fw, &g.bpDirty, 12);
    static_assert(offsetof(State, bpKeyDirty) == offsetof(State, bpDirty) + 1 && offsetof(State, xfKeyDirty) == offsetof(State, bpDirty) + 4 &&
                      offsetof(State, texDirty) == offsetof(State, bpDirty) + 10, "the dirty flags are one run of bytes");
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    const bool otherFlags = ((fw[0] & 0x00FF00FFu) | (fw[1] & 0xFFFFFF00u) | fw[2]) != 0;
#else
    const bool otherFlags = g.bpDirty || g.bpRsDirty || g.xfViewDirty || g.xfProjDirty || g.xfColorDirty || g.xfMtxDirty || g.xfLightDirty || g.texDirty;
#endif
    if (sameKeys && !g_blocksStale && !otherFlags && (g.xfPosRowsDirty & cur.mtxRowsUsed) == 0) {
        // (sameKeys: cur.valid and the vertex-matrix mask unchanged, so the old path's "keys may have changed" is this)
        const bool legacyKeysDirty = g.bpKeyDirty || g.xfKeyDirty || g.xfRegDirty || ((vcdMtxMask & 1u) && shortPalOk != cur.shortPalOk);
        if (legacyKeysDirty && ZTexReplace()) goto full;
        {
        const bool soft = vertexCount <= SoftXformLimit() && SoftEligibleCur();
        if (soft == cur.soft) {
            const bool fold = soft && cur.foldOk && FoldValuesOk();
            if (fold == cur.fold && (!fold || cur.fsIdFoldValid) && (g.tevColDirty & (fold ? 0xF9u : 0xFFu)) == 0) {
                if (legacyKeysDirty) cur.shortPalOk = shortPalOk;
                ClearResolveDirty();
                cur.vcdGen = g.vcdGen;
                return;
            }
        }
        }
    }
full:
    // ---- keys
    const bool legacyKeysDirty = LegacyKeysDirty(vcdMtxMask, shortPalOk);
    const bool vcdChanged = !cur.valid || vcdMtxMask != cur.vcdMtxMask;
    bool fsChanged = false, vsChanged = false;
    if (legacyKeysDirty) cur.shortPalOk = shortPalOk;
    const bool keysChanged = !sameKeys;
    if (keysChanged) ResolveKeysCached(vcdMtxMask, shortPalOk, spo, fsChanged, vsChanged);
    // ---- render state: key registers (cull, blend, pixel format, Z texture mode), viewport, scissor / depth / line width
    // (a depth taken from a Z texture is read when the state is built: built exactly when the old path built it)
    if (keysChanged || g.xfViewDirty || g.bpRsDirty || (legacyKeysDirty && ZTexReplace())) {
        BuildRenderState(cur.rs);
        cur.cullAll = cur.rs.cull == CULL_ALL;
        ++cur.stateSerial;
    }
    // ---- texture bindings: which texmaps the fragment key uses, and what each resolves to
    const bool texRebuild = fsChanged || g.texDirty;
    if (texRebuild) {
        uint8_t mask = 0;
        for (int s = 0; s < cur.fs.numStages; ++s)
            if (cur.fs.stage[s].texEnable) mask |= static_cast<uint8_t>(1u << cur.fs.stage[s].texMap);
        for (int i = 0; i < cur.fs.numIndStages; ++i) mask |= static_cast<uint8_t>(1u << cur.fs.ind[i].texMap);
        // (a texmap outside both the old and the new mask holds an empty binding already)
        const uint32_t visit = static_cast<uint32_t>(mask | cur.texMask);
        cur.texMask = mask;
        ++cur.stateSerial;
        static const bool perSlot = EnvInt("GXV_TEXSLOTS", 1) != 0;
        cur.slotResolved &= static_cast<uint8_t>(~g.texSlotDirty);
        if (!perSlot) cur.slotResolved = 0;
        g.texSlotDirty = 0;
        for (uint32_t vm = visit; vm; vm &= vm - 1) {
            const int m = __builtin_ctz(vm);
            const uint8_t bit = static_cast<uint8_t>(1u << m);
            if (!(mask & bit)) {
                cur.tex[m] = TexBind{};
                cur.texW[m] = cur.texH[m] = 0;
                continue;
            }
            if (!(cur.slotResolved & bit)) {
                cur.slotW[m] = cur.slotH[m] = 0;
                ResolveTexture(m, cur.slotBind[m], cur.slotW[m], cur.slotH[m]);
                cur.slotResolved |= bit;
            }
            cur.tex[m] = cur.slotBind[m];
            cur.texW[m] = cur.slotW[m];
            cur.texH[m] = cur.slotH[m];
        }
    }
    // ---- uniform blocks, each on what it is built from (whether: here; where: the Blk* wrappers, which hand them to the
    // front-end split's finishing thread when it is on)
    const bool soft = vertexCount <= SoftXformLimit() && SoftEligibleCur();
    if (g_blocksStale || g.xfProjDirty) BlkProj();
    if (g_blocksStale || g.xfMtxDirty || (g.xfPosRowsDirty & cur.mtxRowsUsed) != 0 || vsChanged || vcdChanged || soft != cur.soft)
        cur.mtxRowsUsed = BlkMtx(cur.vs, soft);
    cur.soft = soft;
    if (g_blocksStale || g.xfLightDirty || g.xfColorDirty || (vsChanged && (UniformPacking() || LightNorm()))) BlkLight(cur.vs);
    const bool fold = soft && cur.foldOk && FoldValuesOk();
    if (fold && !cur.fsIdFoldValid) {
        cur.fsIdFold = FoldShaderCached();
        cur.fsIdFoldValid = true;
    }
    if (g_blocksStale || g.bpDirty || (g.tevColDirty & (fold ? 0xF9u : 0xFFu)) != 0 || fsChanged || fold != cur.fold || (texRebuild && cur.fs.numIndStages))
        BlkTev(cur.fs, cur.texW, cur.texH, fold);
    cur.fold = fold;
    cur.useFs = fold ? cur.fsIdFold : cur.fsId;
    g_blocksStale = false;
    ClearResolveDirty();
    cur.vcdGen = g.vcdGen;
    cur.valid = true;
}

// GXV_RSCACHE=2: the cached path, then the old one from the same starting point; what they leave is compared.
void ResolveStateLegacy(uint32_t vcdMtxMask, uint32_t vertexCount);
void ResolveStateVerifyBody(uint32_t vcdMtxMask, uint32_t vertexCount);
// With the front-end split on (split::g_on) the blocks are not built here but sent to the finishing thread, which cannot be
// taken back: the two paths are then compared with the split off, building into the player's own arena (unused while the
// split is on, and emptied with the frame), and the old path then runs once more from the same start, split, for the
// picture (GXV_SPLIT=2 checks that part against the single-thread code).
void ResolveStateVerify(uint32_t vcdMtxMask, uint32_t vertexCount) {
    if (!split::g_on) {
        ResolveStateVerifyBody(vcdMtxMask, vertexCount);
        return;
    }
    static Resolved pre;
    pre = cur;
    uint8_t dirty[12];
    std::memcpy(dirty, &g.bpDirty, 12);
    const uint64_t posRows = g.xfPosRowsDirty;
    const uint8_t tevCol = g.tevColDirty, texSlot = g.texSlotDirty;
    const uint32_t prePal = g_pendingPosMtxMax;
    const bool preStale = g_blocksStale;
    split::g_on = false;
    ResolveStateVerifyBody(vcdMtxMask, vertexCount);
    split::g_on = true;
    const CurKey key = g_curKey;
    cur = pre;
    std::memcpy(&g.bpDirty, dirty, 12);
    g.xfPosRowsDirty = posRows;
    g.tevColDirty = tevCol;
    g.texSlotDirty = texSlot;
    g_pendingPosMtxMax = prePal;
    g_blocksStale = preStale;
    ResolveStateLegacy(vcdMtxMask, vertexCount);
    g_curKey = key;
}
void ResolveStateVerifyBody(uint32_t vcdMtxMask, uint32_t vertexCount) {
    struct Flags {
        uint8_t dirty[12];
        uint64_t posRows;
        uint8_t tevCol, texSlot;
        bool operator==(const Flags& o) const {
            return std::memcmp(dirty, o.dirty, 12) == 0 && posRows == o.posRows && tevCol == o.tevCol && texSlot == o.texSlot;
        }
    };
    auto flags = [] {
        Flags f;
        std::memcpy(f.dirty, &g.bpDirty, 12);
        f.posRows = g.xfPosRowsDirty;
        f.tevCol = g.tevColDirty;
        f.texSlot = g.texSlotDirty;
        return f;
    };
    auto setFlags = [](const Flags& f) {
        std::memcpy(&g.bpDirty, f.dirty, 12);
        g.xfPosRowsDirty = f.posRows;
        g.tevColDirty = f.tevCol;
        g.texSlotDirty = f.texSlot;
    };
    static Resolved preCur, newCur; // (static: large)
    static std::vector<float> newUni;
    uint64_t& checked = g_rsChecked;
    uint64_t& bad = g_rsBad;
    preCur = cur;
    uint32_t preOff[UB_COUNT], preLen[UB_COUNT], newOff[UB_COUNT], newLen[UB_COUNT];
    std::memcpy(preOff, g_blockOff, sizeof preOff);
    std::memcpy(preLen, g_blockLen, sizeof preLen);
    const bool preStale = g_blocksStale;
    const size_t preUni = g_buf.uni.size();
    const Flags preFlags = flags();
    const uint32_t prePal = g_pendingPosMtxMax;
    const uint64_t keyHashNow = g.keyHash;

    ResolveStateCached(vcdMtxMask, vertexCount);
    newCur = cur;
    std::memcpy(newOff, g_blockOff, sizeof newOff);
    std::memcpy(newLen, g_blockLen, sizeof newLen);
    newUni.assign(g_buf.uni.begin() + static_cast<std::ptrdiff_t>(preUni), g_buf.uni.end());
    const Flags newFlags = flags();
    const bool newStale = g_blocksStale;
    const CurKey newKey = g_curKey;

    cur = preCur;
    std::memcpy(g_blockOff, preOff, sizeof preOff);
    std::memcpy(g_blockLen, preLen, sizeof preLen);
    g_blocksStale = preStale;
    g_buf.uni.resize(preUni);
    setFlags(preFlags);
    g_pendingPosMtxMax = prePal;
    ResolveStateLegacy(vcdMtxMask, vertexCount);

    // (the bookkeeping that only decides what to rebuild may differ: the merge serial, and the folded shader id while it is not wanted)
    Resolved a = newCur, b = cur;
    a.stateSerial = b.stateSerial = 0;
    if (!a.fsIdFoldValid || !b.fsIdFoldValid) { a.fsIdFold = b.fsIdFold = 0; a.fsIdFoldValid = b.fsIdFoldValid = false; }
    const bool curOk = std::memcmp(&a, &b, sizeof a) == 0;
    const bool blocksOk = std::memcmp(newOff, g_blockOff, sizeof newOff) == 0 && std::memcmp(newLen, g_blockLen, sizeof newLen) == 0 &&
                          newUni.size() == g_buf.uni.size() - preUni &&
                          (newUni.empty() || std::memcmp(newUni.data(), g_buf.uni.data() + preUni, newUni.size() * sizeof(float)) == 0);
    const bool flagsOk = newFlags == flags() && newStale == g_blocksStale;
    const bool hashOk = KeyHashOf(g) == keyHashNow;
    ++checked;
    if (!(curOk && blocksOk && flagsOk && hashOk)) {
        ++bad;
        VGX_LOG_N(16, "rscache: MISMATCH (frame %llu: resolved state %s, blocks %s, flags %s, key hash %s)\n", static_cast<unsigned long long>(g_frame),
                  curOk ? "same" : "DIFFERENT", blocksOk ? "same" : "DIFFERENT", flagsOk ? "same" : "DIFFERENT", hashOk ? "same" : "DIFFERENT");
        g_curKey.valid = false;
    } else {
        g_curKey = newKey; // (cur holds the keys the cached path found)
    }
    if ((checked & 0x3FFFu) == 0)
        VGX_LOG_N(400, "rscache: %llu resolutions checked, %llu mismatching (key memo %llu hits, %llu misses)\n", static_cast<unsigned long long>(checked),
                  static_cast<unsigned long long>(bad), static_cast<unsigned long long>(g_keyMemoHits), static_cast<unsigned long long>(g_keyMemoMisses));
}
// ==================================================================== (end of the resolution cache)
} // namespace
void ResolveCacheCounters(uint64_t out[4]) { out[0] = g_rsChecked; out[1] = g_rsBad; out[2] = g_keyMemoHits; out[3] = g_keyMemoMisses; }
namespace {

inline __attribute__((always_inline)) void ResolveState(uint32_t vcdMtxMask, uint32_t vertexCount) {
    const int mode = RsCacheMode();
    if (mode == 1) ResolveStateCached(vcdMtxMask, vertexCount);
    else if (mode == 0) ResolveStateLegacy(vcdMtxMask, vertexCount);
    else ResolveStateVerify(vcdMtxMask, vertexCount);
}

} // namespace

// GXV_TRACE=<frame> traces that presented frame. GXV_TRACE_DRAWS=<n>
// traces the first frame that follows one with at least n draw records
// (frame numbers are not reproducible between runs: the boot sequence is
// partly wall-clock timed).
static uint64_t g_traceFrame = ~0ull;
static uint32_t g_frameDraws = 0;
int8_t g_traceNow = -1;
bool TraceFrameSlow() {
    static const long f = diag::Int("GXV_TRACE", -1);
    const bool on = diag::kEnabled && ((f >= 0 && static_cast<uint64_t>(f) == g_frame) || g_frame == g_traceFrame);
    g_traceNow = on ? 1 : 0;
    return on;
}

// GXV_TRACE=<frame>: print every draw of that frame with its resolved state.
static void TraceDraw(uint8_t gxPrim, uint32_t layoutMask, uint32_t count, const uint8_t* v) {
    const VtxLayout L = MakeLayout(layoutMask);
    const RenderState& rs = cur.rs;
    std::fprintf(stderr, "[trace] draw prim=0x%02X verts=%u layout=0x%X vs=%u fs=%u stages=%u texgens=%u chans=%u\n", gxPrim, count, layoutMask,
                 cur.vsId, cur.fsId, cur.fs.numStages, cur.fs.numTexGens, cur.vs.numChans);
    std::fprintf(stderr, "[trace]   blend=%u(%u,%u,eq%u) ztest=%u func=%u zwrite=%u cull=%u cw=%u aw=%u sc=(%d,%d,%d,%d) vp=(%.1f,%.1f,%.1f,%.1f) z=(%.4f,%.4f) atest=%u(0x%02X) fog=%u\n",
                 rs.blend, rs.blendSrc, rs.blendDst, rs.blendEq, rs.depthTest, rs.depthFunc, rs.depthWrite, rs.cull, rs.colorWrite, rs.alphaWrite,
                 rs.scX, rs.scY, rs.scW, rs.scH, rs.vpX, rs.vpY, rs.vpW, rs.vpH, rs.zNear, rs.zFar, cur.fs.alphaTestOn, cur.fs.alphaTest, cur.fs.fog);
    for (int s = 0; s < cur.fs.numStages; ++s) {
        const FsKey::Stage& st = cur.fs.stage[s];
        std::fprintf(stderr, "[trace]   stage %d: color=0x%06X alpha=0x%06X tex=%u map=%u coord=%u ras=%u kc=%u ka=%u ind=0x%X\n", s, st.color, st.alpha,
                     st.texEnable, st.texMap, st.texCoord, st.rasChan, st.kcsel, st.kasel, st.ind);
    }
    for (int m = 0; m < 8; ++m)
        if (cur.texMask & (1u << m)) {
            const TexObj& o = g.tex[m].obj;
            std::fprintf(stderr, "[trace]   texmap %d: id=%u %ux%u fmt=0x%X wrap=%u/%u filt=%u/%u mip=%u data=%p tlut=%u\n", m, cur.tex[m].texId, o.width,
                         o.height, o.format, cur.tex[m].wrapS, cur.tex[m].wrapT, cur.tex[m].minFilter, cur.tex[m].magFilter, o.mipmap, o.data, o.tlut);
        }
    for (int i = 0; i < 4; ++i)
        std::fprintf(stderr, "[trace]   chan%d: en=%u matVtx=%u ambVtx=%u diff=%u attn=%u lights=0x%02X\n", i, cur.vs.chan[i].enable, cur.vs.chan[i].matSrcVtx,
                     cur.vs.chan[i].ambSrcVtx, cur.vs.chan[i].diffuse, cur.vs.chan[i].attn, cur.vs.chan[i].lightMask);
    for (int i = 0; i < cur.vs.numTexGens; ++i)
        std::fprintf(stderr, "[trace]   texgen%d: type=%u src=%u proj=%u abc1=%u post=%u pervtx=%u\n", i, cur.vs.tg[i].type, cur.vs.tg[i].src,
                     cur.vs.tg[i].proj, cur.vs.tg[i].inputAbc1, cur.vs.tg[i].post, cur.vs.tg[i].perVtxMtx);
    std::fprintf(stderr, "[trace]   proj type=%u p=(%g,%g,%g,%g,%g,%g) matIdxA=0x%08X tevreg0=(%d,%d,%d,%d) k0=(%d,%d,%d,%d)\n", g.xfReg[0x26],
                 u2f(g.xfReg[0x20]), u2f(g.xfReg[0x21]), u2f(g.xfReg[0x22]), u2f(g.xfReg[0x23]), u2f(g.xfReg[0x24]), u2f(g.xfReg[0x25]), g.matIdxA,
                 g.tevReg[0][0], g.tevReg[0][1], g.tevReg[0][2], g.tevReg[0][3], g.tevKonst[0][0], g.tevKonst[0][1], g.tevKonst[0][2], g.tevKonst[0][3]);
    const uint32_t pi = g.matIdxA & 0x3F;
    std::fprintf(stderr, "[trace]   posmtx[%u]= (%g %g %g %g) (%g %g %g %g) (%g %g %g %g)\n", pi, g.xfPos[pi][0], g.xfPos[pi][1], g.xfPos[pi][2],
                 g.xfPos[pi][3], g.xfPos[pi + 1][0], g.xfPos[pi + 1][1], g.xfPos[pi + 1][2], g.xfPos[pi + 1][3], g.xfPos[pi + 2][0],
                 g.xfPos[pi + 2][1], g.xfPos[pi + 2][2], g.xfPos[pi + 2][3]);
    for (uint32_t i = 0; i < count && i < 4; ++i, v += L.stride) {
        const float* p = reinterpret_cast<const float*>(v);
        std::fprintf(stderr, "[trace]   v%u pos=(%g,%g,%g | %g)", i, p[0], p[1], p[2], p[3]);
        if (L.mask & VL_NRM) { const float* n = reinterpret_cast<const float*>(v + L.offNrm); std::fprintf(stderr, " nrm=(%g,%g,%g)", n[0], n[1], n[2]); }
        if (L.mask & VL_COL0) std::fprintf(stderr, " c0=%02X%02X%02X%02X", v[L.offCol0], v[L.offCol0 + 1], v[L.offCol0 + 2], v[L.offCol0 + 3]);
        if (L.mask & VL_COL1) std::fprintf(stderr, " c1=%02X%02X%02X%02X", v[L.offCol1], v[L.offCol1 + 1], v[L.offCol1 + 2], v[L.offCol1 + 3]);
        for (int t = 0; t < 8; ++t)
            if (L.mask & (VL_TEX0 << t)) { const float* tc = reinterpret_cast<const float*>(v + L.offTex[t]); std::fprintf(stderr, " t%d=(%g,%g)", t, tc[0], tc[1]); }
        std::fprintf(stderr, "\n");
    }
}

CmdBuf& Pending() { return g_buf; }
void SetPendingPosMtxMax(uint32_t row) { g_pendingPosMtxMax = row; }
uint64_t CurrentFrame() { return g_frame; }

// ---- vertex addressing
// A draw's indices count vertices from the start of a buffer, not from the
// draw's own first vertex: the back end then never needs a per-draw byte
// offset in its attribute pointers. That is what the console's GL layer
// requires (sceGxm attribute offsets have 16 bits, and every distinct offset
// costs a newly patched vertex program there; see vgx_backend_gl.cpp), and it
// lets consecutive draws with one vertex layout share one set of pointers
// everywhere. The frame's vertex arena is cut into chunks, each a buffer of
// its own in the back end; inside a chunk every draw's vertices start at a
// multiple of their own stride from the chunk's start (zero padding in
// between), so "first vertex" is a whole number for every layout, and a new
// chunk begins when that number would no longer fit 16-bit indices.
//   GXV_ABSIDX=0   the old scheme: 0-based indices and a byte offset per draw
//                      (what the reference frame hashes were made with)
#ifndef GXV_ABSIDX_DEFAULT
#define GXV_ABSIDX_DEFAULT 1
#endif
#if GXV_ABSIDX_DEFAULT != 0 && GXV_ABSIDX_DEFAULT != 1
#error GXV_ABSIDX_DEFAULT must be 0 or 1
#endif
#if defined(__VGX_VITA__) && GXV_ABSIDX_DEFAULT == 0 && (!defined(GXV_STRICT_DRAW_PROFILE) || GXV_STRICT_DRAW_PROFILE != 1)
#error Relative default indices require the strict vitaGL build profile
#endif
bool AbsoluteIndices() {
    static const bool on = EnvInt("GXV_ABSIDX", GXV_ABSIDX_DEFAULT) != 0;
    return on;
}
static size_t g_chunkStart = 0;    // byte offset in g_buf.vtx of the current chunk
static uint32_t g_pendingFirst = 0; // index of the pending vertices' first one, from the chunk start

// The layout of the mask asked for last (a frame asks for a few masks, tens of thousands of times).
// (a table: register folding asks for two masks in turn for every particle)
static uint32_t g_strideInv[4096]; // ceil(2^32 / stride): n / stride == (n * inv) >> 32 for every byte offset in a chunk
static inline __attribute__((always_inline)) const VtxLayout& LayoutOf(uint32_t layoutMask) {
    static VtxLayout table[4096];
    static uint32_t madeFor[4096]; // layoutMask + 1 of the entry, 0: none yet
    const uint32_t i = layoutMask & 4095u;
    if (madeFor[i] != layoutMask + 1u) {
        table[i] = MakeLayout(layoutMask);
        g_strideInv[i] = static_cast<uint32_t>((0x100000000ull + table[i].stride - 1) / table[i].stride);
        madeFor[i] = layoutMask + 1u;
    }
    return table[i];
}
// Where the last BeginVertices' vertices end, for which layout, and the index of the vertex
// that follows them: the next primitive of the same layout starts right there, which saves
// the division below for every primitive but the first of a run (the processor has no divide
// instruction: 1.1 of a title frame's 13 million instructions were this division).
static size_t g_bvEnd = ~static_cast<size_t>(0);
static uint32_t g_bvMask = 0xFFFFFFFFu, g_bvNext = 0;

// The arena's state before the last BeginVertices, for EmitDraw to take the vertices back and
// lay them out again with colour slots (register folding).
static struct BvSnapshot {
    size_t size = 0, chunkStart = 0, chunks = 0, bvEnd = 0;
    uint32_t bvMask = 0, bvNext = 0;
    size_t pendingStart = ~static_cast<size_t>(0); // where that call's vertices are (~0: no snapshot)
} g_bvSnap;

// Grows the arena. (Front-end split: the finishing thread transforms vertices in the arena in place, so
// its memory may only move while that thread has nothing left to do.)
static inline void ArenaGrow(size_t n) {
    if (split::g_on && n > g_buf.vtx.capacity()) split::BeforeArenaGrow(g_buf.vtx, n);
    g_buf.vtx.resize(n);
}

// Takes back what the last BeginVertices did (the vertices, and the padding in front of them).
static void RestoreBvSnapshot() {
    g_buf.vtx.resize(g_bvSnap.size);
    g_buf.vtxChunks.resize(g_bvSnap.chunks);
    g_chunkStart = g_bvSnap.chunkStart;
    g_bvEnd = g_bvSnap.bvEnd;
    g_bvMask = g_bvSnap.bvMask;
    g_bvNext = g_bvSnap.bvNext;
}

uint8_t* BeginVertices(uint32_t layoutMask, uint32_t count) {
    const VtxLayout& L = LayoutOf(layoutMask);
    size_t pos = g_buf.vtx.size();
    g_bvSnap.size = pos;
    g_bvSnap.chunkStart = g_chunkStart;
    g_bvSnap.chunks = g_buf.vtxChunks.size();
    g_bvSnap.bvEnd = g_bvEnd;
    g_bvSnap.bvMask = g_bvMask;
    g_bvSnap.bvNext = g_bvNext;
    if (AbsoluteIndices()) {
        if (g_buf.vtxChunks.empty()) {
            g_buf.vtxChunks.push_back(0);
            g_chunkStart = 0;
            g_bvEnd = ~static_cast<size_t>(0);
        }
        // (the division by multiplication: a chunk is at most 65,536 vertices of at most 108 bytes)
        size_t first = (pos == g_bvEnd && layoutMask == g_bvMask)
                           ? g_bvNext
                           : static_cast<size_t>((static_cast<uint64_t>(pos - g_chunkStart + L.stride - 1) * g_strideInv[layoutMask & 4095u]) >> 32);
        if (first + count > 0x10000) {
            g_chunkStart = pos;
            g_buf.vtxChunks.push_back(static_cast<uint32_t>(pos));
            first = 0;
        }
        const size_t start = g_chunkStart + first * L.stride;
        if (start != pos) {
            ArenaGrow(start);
            std::memset(g_buf.vtx.data() + pos, 0, start - pos); // (the arena is not zero-filled, and it is hashed)
        }
        pos = start;
        g_pendingFirst = static_cast<uint32_t>(first);
        g_bvMask = layoutMask;
        g_bvNext = static_cast<uint32_t>(first) + count;
        g_bvEnd = pos + static_cast<size_t>(count) * L.stride;
    }
    g_pendingVtxStart = pos;
    g_bvSnap.pendingStart = pos;
    ArenaGrow(g_pendingVtxStart + static_cast<size_t>(count) * L.stride);
    uint8_t* const out = g_buf.vtx.data() + g_pendingVtxStart;
    // Diagnostic guard (console crash at the first frame after a cutscene skip: a store to 0x002F1080,
    // which is not mapped). The vertices must lie in the arena. Otherwise log the arena's state (a
    // few times) and let the caller draw nothing for this call.
    // (the address test is the console's: its user memory starts at 0x80000000; elsewhere, for example the 32-bit
    // Linux harness, ordinary heap memory lies lower, so only a null base counts)
#if defined(__vita__)
    const bool badArena = reinterpret_cast<uintptr_t>(out) < 0x80000000u || g_buf.vtx.data() == nullptr;
#else
    const bool badArena = g_buf.vtx.data() == nullptr && count != 0;
#endif
    if (badArena) {
        VGX_LOG_N(4, "BeginVertices: bad arena pointer %p: pos %" VGX_PRIZU ", data %p, size %" VGX_PRIZU ", cap %" VGX_PRIZU ", chunks %" VGX_PRIZU ", chunkStart %" VGX_PRIZU ", bvEnd %" VGX_PRIZU ", bvNext %u, bvMask %08x, count %u, stride %u, absolute %d\n",
                  static_cast<void*>(out), pos, static_cast<void*>(g_buf.vtx.data()), g_buf.vtx.size(), g_buf.vtx.capacity(), g_buf.vtxChunks.size(),
                  g_chunkStart, g_bvEnd, g_bvNext, g_bvMask, count, static_cast<unsigned>(L.stride), AbsoluteIndices() ? 1 : 0);
        return nullptr;
    }
    return out;
}

// Register folding: the pending vertices (layout `layoutMask`, already transformed) are laid
// out again with both colour slots, which get TEV registers 0 and 1. Returns the new mask,
// or the old one when the vertices cannot be taken back.
static uint32_t FoldRegs(uint32_t layoutMask, uint32_t count) {
    const uint32_t newMask = layoutMask | VL_COL0 | VL_COL1;
    // (every part of a vertex is a whole number of 32-bit words, and the arena is word-aligned)
    uint32_t c0, c1;
    {
        uint8_t b0[4], b1[4];
        for (int c = 0; c < 4; ++c) {
            b0[c] = static_cast<uint8_t>(g.tevReg[1][c]);
            b1[c] = static_cast<uint8_t>(g.tevReg[2][c]);
        }
        std::memcpy(&c0, b0, 4);
        std::memcpy(&c1, b1, 4);
    }
    if (newMask == layoutMask) { // the slots are there (and nothing reads what the game put in them)
        const VtxLayout L = LayoutOf(layoutMask);
        uint8_t* v = g_buf.vtx.data() + g_pendingVtxStart;
        for (uint32_t i = 0; i < count; ++i, v += L.stride) {
            std::memcpy(v + L.offCol0, &c0, 4);
            std::memcpy(v + L.offCol1, &c1, 4);
        }
        return layoutMask;
    }
    static uint32_t tmp[128 * 112 / 4];
    const uint32_t oldWords = LayoutOf(layoutMask).stride / 4u;
    if (g_bvSnap.pendingStart != g_pendingVtxStart || static_cast<size_t>(count) * oldWords > sizeof tmp / 4) return 0xFFFFFFFFu;
    CopyWords(tmp, g_buf.vtx.data() + g_pendingVtxStart, count * oldWords);
    // Take the vertices (and the padding in front of them) back, then ask again.
    RestoreBvSnapshot();
    uint32_t* out = reinterpret_cast<uint32_t*>(BeginVertices(newMask, count));
    if (!out) return 0xFFFFFFFFu; // (BeginVertices' guard refused the arena: draw unfolded)
    // Old vertex: [pos 16][nrm 12]?[col0 4]?[col1 4]?[rest]; new: the same with both colours.
    const uint32_t head = 4u + ((layoutMask & VL_NRM) ? 3u : 0u);
    const uint32_t oldCols = ((layoutMask & VL_COL0) ? 1u : 0u) + ((layoutMask & VL_COL1) ? 1u : 0u);
    const uint32_t rest = oldWords - head - oldCols;
    const uint32_t* in = tmp;
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t k = 0; k < head; ++k) *out++ = in[k];
        *out++ = c0;
        *out++ = c1;
        const uint32_t* r = in + head + oldCols;
        for (uint32_t k = 0; k < rest; ++k) *out++ = r[k];
        in += oldWords;
    }
    return newMask;
}

static inline uint32_t PrimIndexCount(uint8_t gxPrim, uint32_t count) {
    switch (gxPrim & 0xF8) {
    case 0x80: case 0x88: return (count / 4) * 6;
    case 0x90: return (count / 3) * 3;
    case 0x98: case 0xA0: return count >= 3 ? (count - 2) * 3 : 0;
    case 0xA8: return (count / 2) * 2;
    case 0xB0: return count >= 2 ? (count - 1) * 2 : 0;
    case 0xB8: return count;
    default: return 0;
    }
}
uint32_t PrimIndices(uint8_t gxPrim, uint32_t count, uint32_t base, uint16_t* ix) {
    const uint32_t idxCount = PrimIndexCount(gxPrim, count);
    if (!ix || !idxCount) return idxCount;
    const auto B = [base](uint32_t v) { return static_cast<uint16_t>(base + v); };
    switch (gxPrim & 0xF8) {
    case 0x80: case 0x88:
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            *ix++ = B(q); *ix++ = B(q + 1); *ix++ = B(q + 2);
            *ix++ = B(q); *ix++ = B(q + 2); *ix++ = B(q + 3);
        }
        break;
    case 0x90:
        for (uint32_t i = 0; i < idxCount; ++i) *ix++ = B(i);
        break;
    case 0x98:
        for (uint32_t i = 0; i + 2 < count; ++i) {
            if (i & 1) { *ix++ = B(i + 1); *ix++ = B(i); *ix++ = B(i + 2); }
            else { *ix++ = B(i); *ix++ = B(i + 1); *ix++ = B(i + 2); }
        }
        break;
    case 0xA0:
        for (uint32_t i = 0; i + 2 < count; ++i) { *ix++ = B(0); *ix++ = B(i + 1); *ix++ = B(i + 2); }
        break;
    case 0xA8:
        for (uint32_t i = 0; i < idxCount; ++i) *ix++ = B(i);
        break;
    case 0xB0:
        for (uint32_t i = 0; i + 1 < count; ++i) { *ix++ = B(i); *ix++ = B(i + 1); }
        break;
    default:
        for (uint32_t i = 0; i < count; ++i) *ix++ = B(i);
        break;
    }
    return idxCount;
}
static inline uint8_t PrimKind(uint8_t gxPrim) {
    switch (gxPrim & 0xF8) {
    case 0xA8: case 0xB0: return PRIM_LINES;
    case 0xB8: return PRIM_POINTS;
    default: return PRIM_TRIANGLES;
    }
}

// The shader warm-up list: the program and variant of a new record.
void split::NoteVariant(const DrawStateRef& st, uint32_t layoutMask) {
    if (g_shaderList) {
        const uint32_t bk = BlendKey(*st.rs);
        const uint64_t vk = VariantKey(st.vsId, st.useFs, layoutMask, bk);
        if (vk != g_lastVariant) {
#if GXV_RECIPE_SEED_BATCH
            if (g_recipeMerged) PersistRecipeDraw(st.vsId, st.useFs, layoutMask, bk);
            else {
#endif
            g_lastVariant = vk;
            const uint32_t pk = (static_cast<uint32_t>(st.vsId) << 16) | st.useFs;
            if (g_progSeen.insert(pk).second) {
                const uint32_t v[2] = {st.vsId, st.useFs};
                ListAppend(3u, v, sizeof v);
            }
            if (g_varSeen.insert(vk).second) {
                const uint32_t v[4] = {st.vsId, st.useFs, layoutMask & 0xFFFu, bk};
                ListAppend(4u, v, sizeof v);
            }
#if GXV_RECIPE_SEED_BATCH
            }
#endif
        }
    }
}

// Fills a new record with the resolved state (everything except the geometry).
static DrawRec* NewDrawRec(split::FinishCtx& c, uint8_t prim, uint32_t layoutMask, const split::DrawStateRef& st) {
    std::vector<DrawRec>& draws = *c.draws;
    draws.emplace_back();
    DrawRec* rec = &draws.back();
    rec->vsId = st.vsId; rec->fsId = st.useFs;
    rec->prim = prim;
    rec->texMask = st.texMask;
    rec->layoutMask = layoutMask;
    CopyWords(rec->ublock, c.blockOff, UB_COUNT);
    rec->rs = *st.rs;
    rec->exX = st.rs->scX; rec->exY = st.rs->scY; rec->exW = st.rs->scW; rec->exH = st.rs->scH;
    CopyWords(rec->tex, st.tex, static_cast<uint32_t>(sizeof cur.tex / 4));
    *c.lastDraw = static_cast<int>(draws.size()) - 1;
    *c.lastRecSerial = st.serial;
    if (c.cmds) c.cmds->push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(*c.lastDraw)});
    ++*c.records;
    if (c.mainThread) {
        // (the single-thread path: st is cur)
        if (stats::Detailed()) {
            stats::ProgramUsed(cur.vsId, cur.useFs);
            for (int m = 0; m < 8; ++m)
                if ((cur.texMask & (1u << m)) && cur.tex[m].texId) stats::TexBound(cur.tex[m].texId, static_cast<uint32_t>(cur.texW[m]) * cur.texH[m] * 4u);
        }
        split::NoteVariant(st, layoutMask);
    }
    return rec;
}

// The pixels of a small draw (see DrawRec::exX): the bounding box, in EFB pixels, of its
// eye-space vertices under the current projection and viewport. `rect` is set when the draw
// is four vertices that make an axis-aligned rectangle on whole pixels, ordered so that its two
// triangles tile it. False: not known (a vertex behind the eye, values out of range).
static bool SmallDrawBounds(uint8_t gxPrim, const uint8_t* v, uint32_t count, uint32_t stride, EfbRect& box, bool& rect) {
    const RenderState& rs = cur.rs;
    float p[4];
    for (int i = 0; i < 4; ++i) p[i] = u2f(g.xfReg[0x20 + i]);
    const bool ortho = (g.xfReg[0x26] & 1) != 0;
    float px[4], py[4];
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (uint32_t i = 0; i < count; ++i, v += stride) {
        float e[3];
        std::memcpy(e, v, 12);
        float cx, cy, cw;
        if (ortho) { cx = p[0] * e[0] + p[1]; cy = p[2] * e[1] + p[3]; cw = 1.0f; }
        else { cx = p[0] * e[0] + p[1] * e[2]; cy = p[2] * e[1] + p[3] * e[2]; cw = -e[2]; }
        if (!(cw > 1e-6f)) return false;
        const float sx = rs.vpX + (cx / cw + 1.0f) * 0.5f * rs.vpW, sy = rs.vpY + (1.0f - cy / cw) * 0.5f * rs.vpH;
        if (!(sx > -16384.0f && sx < 16384.0f && sy > -16384.0f && sy < 16384.0f)) return false;
        x0 = std::min(x0, sx); x1 = std::max(x1, sx);
        y0 = std::min(y0, sy); y1 = std::max(y1, sy);
        if (i < 4) { px[i] = sx; py[i] = sy; }
    }
    const float e = 0.01f;
    const int bx0 = static_cast<int>(std::floor(x0 + e)), by0 = static_cast<int>(std::floor(y0 + e));
    const int bx1 = static_cast<int>(std::ceil(x1 - e)), by1 = static_cast<int>(std::ceil(y1 - e));
    // (cut to the scissor: nothing is drawn outside it)
    const int ax = std::max<int>(bx0, rs.scX), ay = std::max<int>(by0, rs.scY);
    const int cx1 = std::min<int>(bx1, rs.scX + rs.scW), cy1 = std::min<int>(by1, rs.scY + rs.scH);
    box = EfbRect{static_cast<int16_t>(ax), static_cast<int16_t>(ay), static_cast<int16_t>(cx1 > ax ? cx1 - ax : 0), static_cast<int16_t>(cy1 > ay ? cy1 - ay : 0)};
    rect = false;
    if (count != 4) return true;
    if (std::fabs(x0 - std::floor(x0 + 0.5f)) > e || std::fabs(x1 - std::floor(x1 + 0.5f)) > e || std::fabs(y0 - std::floor(y0 + 0.5f)) > e ||
        std::fabs(y1 - std::floor(y1 + 0.5f)) > e)
        return true;
    int sxn[4], syn[4];
    for (int i = 0; i < 4; ++i) {
        const bool l = std::fabs(px[i] - x0) <= e, r = std::fabs(px[i] - x1) <= e, t = std::fabs(py[i] - y0) <= e, b = std::fabs(py[i] - y1) <= e;
        if (!(l || r) || !(t || b) || (l && r) || (t && b)) return true;
        sxn[i] = r ? 1 : -1;
        syn[i] = b ? 1 : -1;
    }
    const uint8_t kind = gxPrim & 0xF8;
    int d1, o0, o1; // vertex 0's diagonal partner, and the other pair
    if (kind == 0x80 || kind == 0xA0) { d1 = 2; o0 = 1; o1 = 3; }      // quads, fan
    else if (kind == 0x98) { d1 = 3; o0 = 1; o1 = 2; }                 // strip
    else return true;
    if (sxn[0] != -sxn[d1] || syn[0] != -syn[d1] || sxn[o0] != -sxn[o1] || syn[o0] != -syn[o1]) return true;
    if (sxn[0] * syn[0] == sxn[o0] * syn[o0]) return true; // (the two pairs are the two diagonals)
    rect = true;
    return true;
}

// ================================================================ character edge smoothing (GXV_CHARAA)
// the maintainer, 23 Oct 2026: "Can anti aliasing be applied just to the characters and key items to keep costs lower?" The whole
// picture is multisampled only on the top two rungs of the automatic quality (vgx_autoq.h), which the busy scenes never
// reach; this smooths the edges of what the eye follows, at a cost proportional to the area those take on the screen.
// The front end notes where on the EFB the 3D scene's draws of the characters and key items put their pixels: a draw
// is one of those when it samples a texture of the HD pack's list (tools/hdtex/selection.txt: Mario, Luigi, Peach,
// Rosalina, Lumas, Toads, Bowser, Bowser Jr., Power / Grand Stars, the mushrooms and power-ups; IsCharaTexture, from the
// pack's index, whether or not the HD image was uploaded). Its rectangle is the projected bounds of its vertices: for a
// list of the display-list cache, the model-space bounds of its vertices per position-matrix row (worked out once per
// list, RowBoxesOf), through the matrices and the projection of the moment, so a skinned character
// costs a few dozen operations per matrix row of the draw (a bounding sphere per row: ChaProj::Sphere), never a transform per vertex; for other draws (rare: lists the cache
// does not keep) each vertex. Rectangles that meet are merged (Mario's twenty-odd draws make one). The HUD is cut out:
// every draw between the game's PORT_GX_MARK_HUD_BEGIN / END markers (they reach the front end in the GX stream, as a
// write of BP register 0xFF: WriteBP) that is drawn while there are rectangles has its own rectangle subtracted, so no
// HUD pixel is ever filtered. Character draws inside the HUD markers (a 3D star on a menu) are not noted. A rectangle
// wholly inside the source of an EFB copy that clears (an offscreen picture: a reflection, a corner pass) is dropped.
// At the display copy the rectangles, padded by kChaPad pixels for the filter's reach, travel in CopyDispCmd::chara to
// the back end, whose present draws the edge filter over them only (backend::PresentCharaAA), into the display's own
// scene, after the sharpened upscale: no render pass, no GXM scene, no EFB store or reload is added, the EFB itself is
// never changed, and a frame without rectangles issues nothing at all.
//   GXV_CHARAA=0|1|2       off (the old path, exactly: no marker is even written) / on / on, with the pixels the filter
//                          changed tinted red and the rectangles' borders drawn (diagnostic). Default kCharaDefault.
//   GXV_CHARAA_MIN=<px>    least height of a rectangle that is smoothed (default 32 EFB pixels; CharaFinish)
//   GXV_CHARAA_KERNEL=fxaa|sub  the filter (back end, EnsureCharaProgram): FXAA 2 (default) or the Strikers port's sub-pixel step
//   GXV_CHARAA_AREA=<pct>  most of the display copy the rectangles may cover (default 30); a frame above it is not
//                          smoothed (a close-up: the pass would approach a full-screen one, and its edges are long, not jagged)
//   GXV_CHARAA_LOG=<n>     (diagnostic) the rectangles, the HUD rectangles cutting them and the large HUD draws, on stderr,
//                          for n display copies
// HUD draws that cannot change a pixel are not cut out (blended by source alpha with every vertex alpha 0: the layout
// panes the game draws at alpha 0 every frame), nor is one colour over a rectangle holding every character rectangle
// (a fade: the filter commutes with it). Without these two rules the Gateway Galaxy's HUD cut every rectangle away.
// Kernel choice (23 Oct, with the Strikers port's 1.1 SMOOTH filter as the alternative, GXV_CHARAA_KERNEL=sub): on the
// desktop captures FXAA visibly rounds the Lumas' and Mario's stair steps where the sub-pixel step barely changes them;
// a whole-picture filter in the present would also soften the HUD, which is in the EFB by then (the display copy comes
// after the game's 2D).
// Default OFF since the console A/B (24 Oct, Comet Observatory, the same 150 s scripted route): on 41.0 fps average against
// 43.3 off, the front end ("player") 0.66 ms a frame dearer (the Observatory is front-end-bound and has up to 190 character
// draws a frame, the Lumas). The gain on the picture is modest. GXV_CHARAA=1 turns it on.
constexpr int kCharaDefault = 0;
int CharaAAMode() {
    static const int m = [] {
        const long v = EnvInt("GXV_CHARAA", kCharaDefault);
        return v < 0 ? 0 : (v > 2 ? 2 : static_cast<int>(v));
    }();
    return m;
}
namespace {
inline bool ChaOn() {
    static const bool on = CharaAAMode() != 0;
    return on;
}
constexpr int kChaHud = 64, kChaPad = 2, kChaWork = 64;
struct ChaFrame {
    EfbRect box[kCharaBoxes];
    int n = 0;
    EfbRect hud[kChaHud];
    int nh = 0;
    int hudDepth = 0;
    uint32_t serial = ~0u; // cur.stateSerial the decision below was made for
    bool chara = false;    // the current state samples a character texture
    const RowBox* pend = nullptr; // SetPendingBounds
    uint32_t pendN = 0;
} g_cha;
// Totals since the start (CharaCounters): [0] display copies, [1] of them with rectangles, [2] character draws, [3] of
// them with no usable bounds (a vertex behind the eye: not smoothed), [4] HUD draws measured, [5] rectangles handed to
// the back end, [6] their EFB pixels, [7] frames over GXV_CHARAA_AREA, [8] rectangles dropped by clearing copies, [9]
// pieces lost to HUD cuts beyond what CopyDispCmd holds, [10] rectangles under GXV_CHARAA_MIN.
uint64_t g_chaCtr[11] = {};

inline int64_t RArea(const EfbRect& r) { return static_cast<int64_t>(r.w) * r.h; }
inline EfbRect RUnion(const EfbRect& a, const EfbRect& b) {
    const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y), x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
    return EfbRect{static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(x1 - x0), static_cast<int16_t>(y1 - y0)};
}
inline int64_t RInter(const EfbRect& a, const EfbRect& b) {
    const int w = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x), h = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
    return w > 0 && h > 0 ? static_cast<int64_t>(w) * h : 0;
}
inline bool RInside(const EfbRect& a, const EfbRect& o) { return a.x >= o.x && a.y >= o.y && a.x + a.w <= o.x + o.w && a.y + a.h <= o.y + o.h; }
// a minus b: at most four rectangles (bands above and below b, then left and right of it)
int RSubtract(const EfbRect& a, const EfbRect& b, EfbRect out[4]) {
    if (!RInter(a, b)) { out[0] = a; return 1; }
    const int ax1 = a.x + a.w, ay1 = a.y + a.h, bx1 = b.x + b.w, by1 = b.y + b.h;
    const int my0 = std::max<int>(a.y, b.y), my1 = std::min(ay1, by1);
    int n = 0;
    auto put = [&](int x, int y, int w, int h) {
        if (w > 0 && h > 0) out[n++] = EfbRect{static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w), static_cast<int16_t>(h)};
    };
    put(a.x, a.y, a.w, b.y - a.y);
    put(a.x, by1, a.w, ay1 - by1);
    put(a.x, my0, b.x - a.x, my1 - my0);
    put(bx1, my0, ax1 - bx1, my1 - my0);
    return n;
}

// The screen rectangle (EFB pixels) of points given in model space, as the vertex shader projects them: the position
// matrix rows, the projection (XF 0x20..0x26) and the viewport of the current state, cut to its scissor (SmallDrawBounds' sums).
struct ChaProj {
    float p[4];
    bool ortho;
    float vx, vy, vw, vh;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    bool bad = false;
    ChaProj() {
        for (int i = 0; i < 4; ++i) p[i] = u2f(g.xfReg[0x20 + i]);
        ortho = (g.xfReg[0x26] & 1) != 0;
        vx = cur.rs.vpX; vy = cur.rs.vpY; vw = cur.rs.vpW; vh = cur.rs.vpH;
    }
    inline void Eye(float ex, float ey, float ez) {
        float cx, cy, cw;
        if (ortho) { cx = p[0] * ex + p[1]; cy = p[2] * ey + p[3]; cw = 1.0f; }
        else { cx = p[0] * ex + p[1] * ez; cy = p[2] * ey + p[3] * ez; cw = -ez; }
        if (!(cw > 1e-6f)) { bad = true; return; }
        const float iw = 1.0f / cw;
        const float sx = vx + (cx * iw + 1.0f) * 0.5f * vw, sy = vy + (1.0f - cy * iw) * 0.5f * vh;
        x0 = std::min(x0, sx); x1 = std::max(x1, sx);
        y0 = std::min(y0, sy); y1 = std::max(y1, sy);
    }
    inline void Point(const float* m, float x, float y, float z) {
        Eye(m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7], m[8] * x + m[9] * y + m[10] * z + m[11]);
    }
    // A row's bounding sphere through matrix m: its centre in eye space, its radius scaled by the matrix's largest row (the
    // skinning and model matrices are rotations with a scale), and the eye-space box around that sphere projected, its
    // near and far depths taken apart (exact for the box, so conservative for the sphere). About 50 operations a row;
    // the eight corners of the row's box cost five times that (the Observatory: 200 character draws a picture).
    void Sphere(const float* m, const RowBox& b) {
        const float ex = m[0] * b.c[0] + m[1] * b.c[1] + m[2] * b.c[2] + m[3];
        const float ey = m[4] * b.c[0] + m[5] * b.c[1] + m[6] * b.c[2] + m[7];
        const float ez = m[8] * b.c[0] + m[9] * b.c[1] + m[10] * b.c[2] + m[11];
        const float s2 = std::max({m[0] * m[0] + m[1] * m[1] + m[2] * m[2], m[4] * m[4] + m[5] * m[5] + m[6] * m[6], m[8] * m[8] + m[9] * m[9] + m[10] * m[10]});
        const float rr = b.r * std::sqrt(s2);
        float nx0, nx1, ny0, ny1; // NDC extents
        if (ortho) {
            const float a = p[0] * (ex - rr) + p[1], c = p[0] * (ex + rr) + p[1], d = p[2] * (ey - rr) + p[3], e = p[2] * (ey + rr) + p[3];
            nx0 = std::min(a, c); nx1 = std::max(a, c); ny0 = std::min(d, e); ny1 = std::max(d, e);
        } else {
            const float wn = -ez - rr, wf = -ez + rr; // (nearest and farthest distance in front of the eye)
            if (!(wn > 1e-6f)) { bad = true; return; }
            const float in = 1.0f / wn, inf = 1.0f / wf;
            const float xa = p[0] * (ex - rr), xb = p[0] * (ex + rr), ya = p[2] * (ey - rr), yb = p[2] * (ey + rr);
            nx0 = std::min({xa * in, xa * inf, xb * in, xb * inf}) - p[1];
            nx1 = std::max({xa * in, xa * inf, xb * in, xb * inf}) - p[1];
            ny0 = std::min({ya * in, ya * inf, yb * in, yb * inf}) - p[3];
            ny1 = std::max({ya * in, ya * inf, yb * in, yb * inf}) - p[3];
        }
        x0 = std::min(x0, vx + (nx0 + 1.0f) * 0.5f * vw); x1 = std::max(x1, vx + (nx1 + 1.0f) * 0.5f * vw);
        y0 = std::min(y0, vy + (1.0f - ny1) * 0.5f * vh); y1 = std::max(y1, vy + (1.0f - ny0) * 0.5f * vh);
    }
    bool Rect(EfbRect& r) const {
        if (bad || !(x0 <= x1 && y0 <= y1) || !(x0 > -16384.0f && x1 < 16384.0f && y0 > -16384.0f && y1 < 16384.0f)) return false;
        const int bx0 = static_cast<int>(std::floor(x0)), by0 = static_cast<int>(std::floor(y0));
        const int bx1 = static_cast<int>(std::ceil(x1)), by1 = static_cast<int>(std::ceil(y1));
        const RenderState& rs = cur.rs;
        const int ax = std::max<int>(bx0, rs.scX), ay = std::max<int>(by0, rs.scY);
        const int cx1 = std::min<int>(bx1, rs.scX + rs.scW), cy1 = std::min<int>(by1, rs.scY + rs.scH);
        r = EfbRect{static_cast<int16_t>(ax), static_cast<int16_t>(ay), static_cast<int16_t>(cx1 > ax ? cx1 - ax : 0), static_cast<int16_t>(cy1 > ay ? cy1 - ay : 0)};
        return true;
    }
};
inline const float* ChaRows(uint32_t row) { return g.xfPos[row < 62 ? row : 61]; }
// The draw's rectangle: from the list's bounds when given (pb, pn), else from its vertices (v: decoded, model space).
bool ChaBounds(const uint8_t* v, uint32_t count, uint32_t stride, const RowBox* pb, uint32_t pn, EfbRect& r) {
    ChaProj P;
    const bool perVtx = cur.vs.perVtxPos != 0;
    if (pb && pn) {
        if (!perVtx) P.Sphere(ChaRows(g.matIdxA & 0x3F), pb[0]);
        else
            for (uint32_t i = 1; i < pn && !P.bad; ++i) P.Sphere(ChaRows(pb[i].row), pb[i]);
    } else if (v && count) {
        const float* m = ChaRows(g.matIdxA & 0x3F);
        for (uint32_t i = 0; i < count && !P.bad; ++i, v += stride) {
            float q[4];
            std::memcpy(q, v, sizeof q);
            if (perVtx) m = ChaRows(static_cast<uint32_t>(q[3] + 0.5f));
            P.Point(m, q[0], q[1], q[2]);
        }
    } else {
        return false;
    }
    return P.Rect(r);
}
void ChaAddBox(EfbRect r) {
    // (merged with a rectangle it meets when the union wastes little: overlapping parts of one model, not two models apart)
    for (bool again = true; again;) {
        again = false;
        for (int i = 0; i < g_cha.n; ++i) {
            const EfbRect& b = g_cha.box[i];
            if (!(b.x <= r.x + r.w + 4 && r.x <= b.x + b.w + 4 && b.y <= r.y + r.h + 4 && r.y <= b.y + b.h + 4)) continue;
            const EfbRect u = RUnion(b, r);
            if (RArea(u) * 2 > (RArea(b) + RArea(r) - RInter(b, r)) * 3 && !RInside(r, b) && !RInside(b, r)) continue;
            r = u;
            g_cha.box[i] = g_cha.box[--g_cha.n];
            again = true;
            break;
        }
    }
    if (g_cha.n == kCharaBoxes) { // (full: into the rectangle it grows least)
        int best = 0;
        int64_t grow = INT64_MAX;
        for (int i = 0; i < g_cha.n; ++i) {
            const int64_t d = RArea(RUnion(g_cha.box[i], r)) - RArea(g_cha.box[i]);
            if (d < grow) { grow = d; best = i; }
        }
        g_cha.box[best] = RUnion(g_cha.box[best], r);
        return;
    }
    g_cha.box[g_cha.n++] = r;
}
void ChaAddHud(const EfbRect& r) {
    if (r.w <= 0 || r.h <= 0) return;
    bool hits = false;
    for (int i = 0; i < g_cha.n && !hits; ++i) hits = RInter(g_cha.box[i], r) > 0;
    if (!hits) return; // (a HUD element away from every character: nothing to cut)
    if (g_cha.nh < kChaHud) g_cha.hud[g_cha.nh++] = r;
    else g_cha.hud[kChaHud - 1] = RUnion(g_cha.hud[kChaHud - 1], r); // (conservative: cuts more)
}
// After ResolveState in the emitters (EmitDraw: before its vertices are transformed on the CPU).
void CharaNote(const uint8_t* v, uint32_t count, uint32_t stride, uint32_t layoutMask) {
    const RowBox* pb = g_cha.pend;
    const uint32_t pn = g_cha.pendN;
    g_cha.pend = nullptr;
    g_cha.pendN = 0;
    if (g_cha.hudDepth > 0) {
        if (g_cha.n == 0 || !cur.rs.colorWrite) return; // (no rectangle to cut / the picture's colour is not written)
        ++g_chaCtr[4];
        // A HUD draw that cannot change a pixel does not cut: the game draws some layout panes every frame with their alpha
        // at 0 (blended by source alpha, the pane's alpha in the vertex colours, as nw4r's layouts carry it). The Gateway
        // tutorial on the desktop build: 92 HUD draws a frame, among them full-picture and 222x280 panes of alpha 0.
        if (v && cur.rs.blend && cur.rs.blendSrc == BF_SRC_ALPHA && (layoutMask & VL_COL0)) {
            const uint8_t* a = v + MakeLayout(layoutMask).offCol0 + 3;
            bool none = true;
            for (uint32_t i = 0; i < count && none; ++i) none = a[static_cast<size_t>(i) * stride] == 0;
            if (none) return;
        }
        EfbRect r;
        const bool known = ChaBounds(v, count, stride, pb, pn, r);
        if (!known) r = EfbRect{cur.rs.scX, cur.rs.scY, cur.rs.scW, cur.rs.scH}; // (unknown: all it may touch)
        // One colour over a rectangle that holds every character rectangle (no texture, no vertex colours: a fade, which the
        // game draws over the whole picture every frame, mostly at alpha 0): the same blend of one colour over every pixel the
        // filter reads, which the filter commutes with (but for its edge threshold), so it does not cut either.
        if (cur.texMask == 0 && !(layoutMask & (VL_COL0 | VL_COL1)) && v) {
            bool all = true;
            for (int i = 0; i < g_cha.n && all; ++i) {
                const EfbRect& b = g_cha.box[i];
                all = RInside(EfbRect{static_cast<int16_t>(b.x - kChaPad - 2), static_cast<int16_t>(b.y - kChaPad - 2), static_cast<int16_t>(b.w + 2 * kChaPad + 4),
                                      static_cast<int16_t>(b.h + 2 * kChaPad + 4)}, r);
            }
            if (all) return;
        }
        static long hudLog = EnvInt("GXV_CHARAA_LOG", 0) * 4;
        if (hudLog > 0 && r.w * r.h > 20000) {
            --hudLog;
            int amin = 999, amax = -1;
            if (v && (layoutMask & VL_COL0)) {
                const VtxLayout L = MakeLayout(layoutMask);
                for (uint32_t i = 0; i < count; ++i) { const int a = v[i * stride + L.offCol0 + 3]; amin = std::min(amin, a); amax = std::max(amax, a); }
            }
            std::fprintf(stderr, "[charaa] HUD draw (%d,%d %dx%d)%s: %u vertices%s, ortho %u, viewport %.0f,%.0f %.0fx%.0f, tex %u, blend %u %u/%u, cw %u aw %u, a %d..%d\n", r.x, r.y, r.w, r.h,
                         known ? "" : " (unknown bounds: scissor)", count, pb ? " (list bounds)" : "", g.xfReg[0x26] & 1, cur.rs.vpX, cur.rs.vpY,
                         cur.rs.vpW, cur.rs.vpH, cur.texMask ? cur.tex[__builtin_ctz(cur.texMask)].texId : 0u, cur.rs.blend, cur.rs.blendSrc, cur.rs.blendDst,
                         cur.rs.colorWrite, cur.rs.alphaWrite, amin, amax);
        }
        ChaAddHud(r);
        return;
    }
    if (cur.stateSerial != g_cha.serial) {
        g_cha.serial = cur.stateSerial;
        bool c = false;
        for (uint32_t m = cur.texMask; m && !c; m &= m - 1) c = IsCharaTexture(cur.tex[__builtin_ctz(m)].texId);
        g_cha.chara = c;
    }
    if (!g_cha.chara || !cur.rs.colorWrite) return;
    ++g_chaCtr[2];
    EfbRect r;
    if (!ChaBounds(v, count, stride, pb, pn, r)) { ++g_chaCtr[3]; return; }
    if (r.w > 0 && r.h > 0) ChaAddBox(r);
}
// An EFB copy that clears `src`: what was drawn wholly inside it is not on the picture.
void CharaCleared(const EfbRect& src) {
    for (int i = 0; i < g_cha.n;)
        if (RInside(g_cha.box[i], src)) { g_cha.box[i] = g_cha.box[--g_cha.n]; ++g_chaCtr[8]; }
        else ++i;
}
void CharaFinish(CopyDispCmd& c) {
    static const int64_t maxPct = std::clamp<long>(EnvInt("GXV_CHARAA_AREA", 30), 1, 100);
    ++g_chaCtr[0];
    g_cha.hudDepth = 0;
    c.charaN = 0;
    if (g_cha.n == 0) { g_cha.nh = 0; return; }
    ++g_chaCtr[1];
    EfbRect work[kChaWork], next[kChaWork];
    int nw = 0;
    // GXV_CHARAA_MIN=<px> (default 32): a rectangle less tall than this many EFB pixels is not smoothed. A character that
    // small (a far Luma, a Star Bit-sized item) is all texture detail and few stair steps, and the filter would only soften
    // it (desktop captures, 23 Oct: a small Mario's face and overalls came out blurred with FXAA's looser thresholds).
    static const int minH = static_cast<int>(std::clamp<long>(EnvInt("GXV_CHARAA_MIN", 32), 0, 456));
    for (int i = 0; i < g_cha.n; ++i) {
        const EfbRect& b = g_cha.box[i];
        if (b.h < minH) { ++g_chaCtr[10]; continue; }
        const int x0 = std::max<int>(b.x - kChaPad, c.src.x), y0 = std::max<int>(b.y - kChaPad, c.src.y);
        const int x1 = std::min<int>(b.x + b.w + kChaPad, c.src.x + c.src.w), y1 = std::min<int>(b.y + b.h + kChaPad, c.src.y + c.src.h);
        if (x1 > x0 && y1 > y0) work[nw++] = EfbRect{static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(x1 - x0), static_cast<int16_t>(y1 - y0)};
    }
    // GXV_CHARAA_LOG=<n> (diagnostic): the rectangles and the HUD rectangles cutting them, for n display copies with rectangles
    static long logLeft = EnvInt("GXV_CHARAA_LOG", 0);
    if (logLeft > 0) {
        --logLeft;
        std::fprintf(stderr, "[charaa] copy %llu: %d boxes:", static_cast<unsigned long long>(g_chaCtr[0]), nw);
        for (int i = 0; i < nw; ++i) std::fprintf(stderr, " (%d,%d %dx%d)", work[i].x, work[i].y, work[i].w, work[i].h);
        std::fprintf(stderr, "; %d HUD:", g_cha.nh);
        for (int i = 0; i < g_cha.nh; ++i) std::fprintf(stderr, " (%d,%d %dx%d)", g_cha.hud[i].x, g_cha.hud[i].y, g_cha.hud[i].w, g_cha.hud[i].h);
        std::fprintf(stderr, "\n");
    }
    for (int h = 0; h < g_cha.nh && nw; ++h) {
        int nn = 0;
        for (int i = 0; i < nw; ++i) {
            EfbRect piece[4];
            const int k = RSubtract(work[i], g_cha.hud[h], piece);
            for (int j = 0; j < k; ++j) {
                if (piece[j].w < 2 || piece[j].h < 2) continue; // (slivers: nothing to see)
                if (nn < kChaWork) next[nn++] = piece[j];
                else ++g_chaCtr[9]; // (dropped: not smoothed, never a HUD pixel smoothed)
            }
        }
        std::memcpy(work, next, sizeof(EfbRect) * nn);
        nw = nn;
    }
    g_cha.n = g_cha.nh = 0;
    if (!nw) return;
    // (the largest first, as many as the command holds)
    std::sort(work, work + nw, [](const EfbRect& a, const EfbRect& b) { return RArea(a) > RArea(b); });
    if (nw > kCharaBoxes) { g_chaCtr[9] += static_cast<uint64_t>(nw - kCharaBoxes); nw = kCharaBoxes; }
    int64_t area = 0;
    for (int i = 0; i < nw; ++i) area += RArea(work[i]);
    if (area * 100 > RArea(c.src) * maxPct) { ++g_chaCtr[7]; return; }
    for (int i = 0; i < nw; ++i) c.chara[i] = work[i];
    c.charaN = static_cast<uint8_t>(nw);
    g_chaCtr[5] += static_cast<uint64_t>(nw);
    g_chaCtr[6] += static_cast<uint64_t>(area);
}
} // namespace

void SetPendingBounds(const RowBox* boxes, uint32_t n) {
    g_cha.pend = boxes;
    g_cha.pendN = n;
}
void RowBoxesOf(const uint8_t* verts, uint32_t count, uint32_t stride, std::vector<RowBox>& out) {
    if (!count) return;
    if (out.empty()) out.push_back(RowBox{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}, 0, {}, 0});
    size_t last = 0;
    for (uint32_t i = 0; i < count; ++i, verts += stride) {
        float q[4];
        std::memcpy(q, verts, sizeof q);
        const uint32_t row = q[3] > 0.0f ? static_cast<uint32_t>(q[3] + 0.5f) : 0u;
        if (last == 0 || out[last].row != row) {
            last = 0;
            for (size_t k = 1; k < out.size(); ++k)
                if (out[k].row == row) { last = k; break; }
            if (last == 0) {
                out.push_back(RowBox{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}, row, {}, 0});
                last = out.size() - 1;
            }
        }
        for (int a = 0; a < 3; ++a) {
            out[0].mn[a] = std::min(out[0].mn[a], q[a]); out[0].mx[a] = std::max(out[0].mx[a], q[a]);
            out[last].mn[a] = std::min(out[last].mn[a], q[a]); out[last].mx[a] = std::max(out[last].mx[a], q[a]);
        }
    }
    for (RowBox& b : out) {
        float d2 = 0;
        for (int a = 0; a < 3; ++a) { b.c[a] = (b.mn[a] + b.mx[a]) * 0.5f; d2 += (b.mx[a] - b.mn[a]) * (b.mx[a] - b.mn[a]); }
        b.r = 0.5f * std::sqrt(d2);
    }
}
void CharaMarker(uint32_t kind) {
    if (kind == 1) ++g_cha.hudDepth;                                   // PORT_GX_MARK_HUD_BEGIN
    else if (kind == 2 && g_cha.hudDepth > 0) --g_cha.hudDepth;         // PORT_GX_MARK_HUD_END
}
void CharaCounters(uint64_t out[11]) { std::memcpy(out, g_chaCtr, sizeof g_chaCtr); }

// Register folding on a run of particles: every one of them used to lay its vertices out again with two colour
// slots (FoldRegs), hundreds of instructions for a quad. FoldGuess says, before the vertices are decoded, whether
// the draw will fold, from the state ResolveState left and the registers written since: while no shader key, no
// vertex descriptor and no matrix-index mode has changed, the only things that decide it are the vertex count
// and the two TEV registers' values. A draw that follows such a draw is decoded into the folded layout at once.
// (If it does not fold after all, EmitDraw gives the reservation back and the caller decodes again.)
uint32_t FoldGuess(uint32_t layoutMask, uint32_t count) {
    static const bool on = EnvInt("GXV_FOLDAHEAD", 1) != 0;
    const uint32_t slots = VL_COL0 | VL_COL1;
    if (!on || (layoutMask & slots) == slots) return layoutMask; // (the slots are there)
    // (ResolveState rebuilds the keys when any of these is set: no prediction then)
    if (!cur.valid || g.bpKeyDirty || g.xfKeyDirty || g.xfRegDirty) return layoutMask;
    // (the descriptor as ResolveState saw it, and no matrix index per vertex: that case has the short palette test)
    if (g.vcdGen != cur.vcdGen || (cur.vcdMtxMask & 1u)) return layoutMask;
    if (count > SoftXformLimit() || !cur.foldOk || !SoftEligible(cur.vs) || !FoldValuesOk()) return layoutMask;
    return layoutMask | slots;
}

// The record part of a draw: merged into the previous record when nothing but the geometry differs, else a new
// record; then the indices. (The front-end split runs this on its finishing thread: vgx_fesplit.h.)
uint32_t split::FinishDraw(FinishCtx& c, const DrawJob& j, const DrawStateRef& st) {
    // (GXV_NOMERGE=1 turns merging off: one record per primitive, for comparing runs.)
    static const bool noMerge = EnvInt("GXV_NOMERGE", 0) != 0;
    std::vector<DrawRec>& draws = *c.draws;
    std::vector<uint16_t, NoInitAlloc<uint16_t>>& idx = *c.idx;
    DrawRec* rec = nullptr;
    uint32_t made = ~0u;
    const bool abs = AbsoluteIndices();
    const uint16_t chunk = static_cast<uint16_t>(j.chunk);
    uint32_t base = abs ? j.first : 0; // what the new primitive's 0-based indices are offset by
    const int last = *c.lastDraw;
    const bool prevIsDraw = c.cmds ? (!c.cmds->empty() && c.cmds->back().type == CMD_DRAW) : j.prevIsDraw != 0;
    if (!noMerge && last >= 0 && last == static_cast<int>(draws.size()) - 1 && prevIsDraw) {
        DrawRec& p = draws[last];
        // (the new vertices follow the record's last one: by index within the same chunk, or by byte offset)
        const bool contiguous = abs ? (p.vchunk == chunk && p.vtxFirst + p.vtxCount == j.first)
                                    : (p.vtxOffset + static_cast<size_t>(p.vtxCount) * j.stride == j.vtxStart);
        // (the render state, textures and texture mask are the record's when they have not been built again since it was made:
        // GXV_MERGEFAST=0 compares them always, 2 compares them as well and logs a disagreement)
        static const int mergeMode = static_cast<int>(EnvInt("GXV_MERGEFAST", 1));
        const bool sameSerial = mergeMode != 0 && *c.lastRecSerial == st.serial;
        const auto sameState = [&] {
            return p.texMask == st.texMask && SameStruct(p.rs, *st.rs) && SameWords(p.tex, st.tex, static_cast<uint32_t>(sizeof cur.tex / 4));
        };
        if (diag::kEnabled && mergeMode == 2) {
            static uint64_t checked = 0, bad = 0;
            ++checked;
            if (sameSerial && !sameState()) { ++bad; VGX_LOG_N(16, "merge: MISMATCH (same serial, different state)\n"); }
            if ((checked & 0xFFFFF) == 0) VGX_LOG_N(200, "merge: %llu merge tests checked, %llu mismatching\n", static_cast<unsigned long long>(checked), static_cast<unsigned long long>(bad));
        }
        if (p.vbuf == 0 && p.prim == j.prim && p.layoutMask == j.layoutMask && p.vsId == st.vsId && p.fsId == st.useFs &&
            p.vtxCount + j.count <= 0xFFFF && contiguous &&
            p.idxOffset + p.idxCount == idx.size() && SameWords(p.ublock, c.blockOff, UB_COUNT) && (sameSerial || sameState())) {
            rec = &p;
            *c.lastRecSerial = st.serial;
            if (!abs) base = p.vtxCount;
        }
    }
    if (!rec) {
        rec = NewDrawRec(c, j.prim, j.layoutMask, st);
        made = static_cast<uint32_t>(draws.size() - 1);
        if (abs) {
            rec->vtxOffset = 0;
            rec->vtxFirst = j.first;
            rec->vchunk = chunk;
        } else {
            rec->vtxOffset = j.vtxStart;
        }
        rec->idxOffset = static_cast<uint32_t>(idx.size());
        rec->exX = j.exX; rec->exY = j.exY; rec->exW = j.exW; rec->exH = j.exH;
        rec->cover = j.cover;
    } else if (j.exX != rec->exX || j.exY != rec->exY || j.exW != rec->exW || j.exH != rec->exH) {
        // (merged: the record's bound is the union)
        const int ax = std::min<int>(j.exX, rec->exX), ay = std::min<int>(j.exY, rec->exY);
        const int bx = std::max<int>(j.exX + j.exW, rec->exX + rec->exW), by = std::max<int>(j.exY + j.exH, rec->exY + rec->exH);
        rec->exX = static_cast<int16_t>(ax); rec->exY = static_cast<int16_t>(ay);
        rec->exW = static_cast<int16_t>(bx - ax); rec->exH = static_cast<int16_t>(by - ay);
    }
    const size_t io = idx.size();
    idx.resize(io + j.idxCount);
    PrimIndices(j.gxPrim, j.count, base, idx.data() + io);
    rec->vtxCount += j.count;
    rec->idxCount += j.idxCount;
    return made;
}

uint32_t split::FinishStatic(FinishCtx& c, const DrawJob& j, const DrawStateRef& st) {
    DrawRec* rec = NewDrawRec(c, j.prim, j.layoutMask, st);
    rec->vbuf = j.vbuf;
    rec->vtxOffset = j.vtxOffset;
    rec->vtxFirst = j.vtxFirst;
    rec->idxOffset = j.idxOffset;
    rec->vtxCount = j.count;
    rec->idxCount = j.idxCount;
    return static_cast<uint32_t>(c.draws->size() - 1);
}

static inline split::FinishCtx MainFinishCtx() {
    return split::FinishCtx{&g_buf.draws, &g_buf.idx, &g_buf.cmds, g_blockOff, &g_lastDraw, &g_lastRecSerial, &stats::Cur().drawCalls, true};
}
static inline split::DrawStateRef CurStateRef() { return split::DrawStateRef{cur.vsId, cur.useFs, cur.texMask, cur.stateSerial, &cur.rs, cur.tex}; }
// The record part of a draw: here, or (front-end split) a placeholder command and a job for the finishing thread.
static inline void FinishOrSend(const split::DrawJob& j) {
    const split::DrawStateRef st = CurStateRef();
    if (split::g_on) {
        // (the warm-up list's variant of a draw depends on these only (the blend key on the render state, whose serial
        // this is): most draws repeat the last one's)
        static struct { uint32_t ids = ~0u, layout = 0, serial = 0; } last;
        const uint32_t ids = (static_cast<uint32_t>(st.vsId) << 16) | st.useFs;
        if (GXV_RECIPE_SEED_BATCH || ids != last.ids || j.layoutMask != last.layout || st.serial != last.serial) {
            last.ids = ids;
            last.layout = j.layoutMask;
            last.serial = st.serial;
            split::NoteVariant(st, j.layoutMask);
        }
        split::DrawJob jj = j;
        jj.prevIsDraw = g_lastDraw >= 0 && !g_buf.cmds.empty() && g_buf.cmds.back().type == CMD_DRAW;
        g_buf.cmds.push_back(Cmd{CMD_DRAW, split::Job(jj, st)});
        g_lastDraw = 0; // (some record is the last command's; which one the finishing thread decides)
        return;
    }
    split::FinishCtx c = MainFinishCtx();
    if (j.kind == split::JOB_STATIC) split::FinishStatic(c, j, st);
    else split::FinishDraw(c, j, st);
}

bool EmitDraw(uint8_t gxPrim, uint32_t layoutMask, uint32_t count, uint32_t foldedFrom) {
    VtxLayout L = LayoutOf(layoutMask);
    g_buf.vtx.resize(g_pendingVtxStart + static_cast<size_t>(count) * L.stride);
    auto drop = [&] { g_buf.vtx.resize(g_pendingVtxStart); };
    if (count == 0) return true;

    const uint8_t prim = PrimKind(gxPrim);
    const uint32_t idxCount = PrimIndexCount(gxPrim, count);
    if (idxCount == 0) { drop(); return true; }

    uint32_t vcdMtxMask = 0;
    if (cur.valid && g.vcdGen == cur.vcdGen) {
        vcdMtxMask = cur.vcdMtxMask; // (the descriptor has not changed since ResolveState read it)
    } else {
        for (int i = 0; i <= 8; ++i)
            if (g.vcd[i]) vcdMtxMask |= 1u << i;
    }
    ResolveState(vcdMtxMask, count);
    if (foldedFrom != 0xFFFFFFFFu && !cur.fold) {
        // (laid out for folding, and it does not fold: undo the reservation, the caller decodes again)
        RestoreBvSnapshot();
        g_bvSnap.pendingStart = ~static_cast<size_t>(0);
        return false;
    }
    if (cur.cullAll && prim == PRIM_TRIANGLES) { drop(); return true; }
    if (ChaOn()) CharaNote(g_buf.vtx.data() + g_pendingVtxStart, count, L.stride, L.mask); // (GXV_CHARAA: model-space vertices still)
    // (front-end split: the finishing thread transforms the vertices, once they are where they stay, below)
    const bool split = split::g_on;
    if (cur.soft && !split) SoftTransform(g_buf.vtx.data() + g_pendingVtxStart, count, L);
    // Where its pixels are, for the bookkeeping of the EFB's contents (pass isolation): worked out
    // for the small draws that are not depth-tested (screen-space quads: post-processing, HUD).
    EfbRect ext{cur.rs.scX, cur.rs.scY, cur.rs.scW, cur.rs.scH};
    uint8_t cover = 0;
    if (cur.soft && prim == PRIM_TRIANGLES && count <= 16 && (!cur.rs.depthTest || cur.rs.depthFunc == CMP_ALWAYS)) {
        EfbRect box;
        bool rect = false;
        const uint8_t* bv = g_buf.vtx.data() + g_pendingVtxStart;
        alignas(16) uint8_t eye[16 * 112]; // (split: a transformed copy; 16 vertices of at most 108 bytes)
        if (split) {
            std::memcpy(eye, bv, static_cast<size_t>(count) * L.stride);
            SoftTransform(eye, count, L);
            bv = eye;
        }
        if (SmallDrawBounds(gxPrim, bv, count, L.stride, box, rect)) {
            ext = box;
            const RenderState& rs = cur.rs;
            cover = (rect && !rs.blend && rs.colorWrite && !cur.fs.alphaTestOn && rs.cull == CULL_NONE && box.w > 0 && box.h > 0) ? 1 : 0;
        }
    }
    if (cur.fold) {
        const uint32_t m = FoldRegs(layoutMask, count);
        if (m == 0xFFFFFFFFu) {
            // (cannot happen with the callers there are: draw it unfolded, with the registers in the block)
            cur.fold = false;
            cur.useFs = cur.fsId;
            BlkTev(cur.fs, cur.texW, cur.texH, false);
        } else if (m != layoutMask) {
            layoutMask = m;
            L = LayoutOf(layoutMask);
        }
    }
    g_bvSnap.pendingStart = ~static_cast<size_t>(0);
    // (a record holds at most 65,535 vertices: such a primitive cannot be merged either)
    if (count > 0xFFFF) { drop(); return true; }
    if (cur.soft && split) split::Xform(g_pendingVtxStart, count, layoutMask);

    split::DrawJob j;
    j.kind = split::JOB_DYNAMIC;
    j.gxPrim = gxPrim;
    j.prim = prim;
    j.cover = cover;
    j.layoutMask = layoutMask;
    j.count = count;
    j.idxCount = idxCount;
    const bool abs = AbsoluteIndices();
    j.chunk = abs ? static_cast<uint16_t>(g_buf.vtxChunks.size() - 1) : 0;
    j.first = abs ? g_pendingFirst : 0;
    j.vtxStart = static_cast<uint32_t>(g_pendingVtxStart);
    j.stride = L.stride;
    j.exX = ext.x; j.exY = ext.y; j.exW = ext.w; j.exH = ext.h;
    FinishOrSend(j);
    ++g_frameDraws;
    {
        stats::Frame& fr = stats::Cur();
        ++fr.prims;
        fr.verts += count;
        if (prim == PRIM_TRIANGLES) fr.tris += idxCount / 3;
        if (cur.soft) fr.softVerts += count;
    }
    if (stats::Detailed()) stats::Draw(cur.fs, cur.vs, layoutMask, count);
    if (TraceFrame()) TraceDraw(gxPrim, layoutMask, count, g_buf.vtx.data() + g_pendingVtxStart);
    return true;
}

void EmitStaticDraw(uint8_t gxPrim, uint32_t layoutMask, uint32_t count, uint32_t bufId, uint32_t vtxByteOffset, uint32_t idxOffset,
                    uint32_t vtxFirst) {
    const uint8_t prim = PrimKind(gxPrim);
    const uint32_t idxCount = PrimIndexCount(gxPrim, count);
    if (count == 0 || idxCount == 0 || count > 0xFFFF) return;
    uint32_t vcdMtxMask = 0;
    for (int i = 0; i <= 8; ++i)
        if (g.vcd[i]) vcdMtxMask |= 1u << i;
    // Larger than the CPU-transform limit by construction (see vgx_dlcache.cpp), so never "soft".
    ResolveState(vcdMtxMask, 0xFFFFFFFFu);
    if (cur.cullAll && prim == PRIM_TRIANGLES) return;
    if (ChaOn()) CharaNote(nullptr, 0, 0, 0); // (GXV_CHARAA: the list's bounds, SetPendingBounds)
    split::DrawJob j;
    j.kind = split::JOB_STATIC;
    j.gxPrim = gxPrim;
    j.prim = prim;
    j.layoutMask = layoutMask;
    j.count = count;
    j.idxCount = idxCount;
    j.vbuf = bufId;
    j.vtxOffset = vtxByteOffset;
    j.vtxFirst = vtxFirst;
    j.idxOffset = idxOffset;
    FinishOrSend(j);
    ++g_frameDraws;
    {
        stats::Frame& fr = stats::Cur();
        ++fr.prims;
        fr.verts += count;
        ++fr.staticDraws;
        fr.staticVerts += count;
        if (prim == PRIM_TRIANGLES) fr.tris += idxCount / 3;
    }
    if (stats::Detailed()) stats::Draw(cur.fs, cur.vs, layoutMask, count);
}

void EmitStaticRun(uint8_t prim, uint32_t layoutMask, uint32_t vtxCount, uint32_t idxCount, uint32_t prims, uint32_t bufId, uint32_t vtxByteOffset,
                   uint32_t idxOffset, uint32_t vtxFirst) {
    if (vtxCount == 0 || idxCount == 0 || vtxCount > 0xFFFF) return;
    uint32_t vcdMtxMask = 0;
    for (int i = 0; i <= 8; ++i)
        if (g.vcd[i]) vcdMtxMask |= 1u << i;
    ResolveState(vcdMtxMask, 0xFFFFFFFFu); // (static geometry is transformed by the vertex shader)
    if (cur.cullAll && prim == PRIM_TRIANGLES) return;
    if (ChaOn()) CharaNote(nullptr, 0, 0, 0); // (GXV_CHARAA: the list's bounds, SetPendingBounds)
    split::DrawJob j;
    j.kind = split::JOB_STATIC;
    j.prim = prim;
    j.layoutMask = layoutMask;
    j.count = vtxCount;
    j.idxCount = idxCount;
    j.vbuf = bufId;
    j.vtxOffset = vtxByteOffset;
    j.vtxFirst = vtxFirst;
    j.idxOffset = idxOffset;
    FinishOrSend(j);
    g_frameDraws += prims;
    {
        stats::Frame& fr = stats::Cur();
        fr.prims += prims;
        fr.verts += vtxCount;
        ++fr.staticDraws;
        fr.staticVerts += vtxCount;
        if (prim == PRIM_TRIANGLES) fr.tris += idxCount / 3;
    }
    if (stats::Detailed()) stats::Draw(cur.fs, cur.vs, layoutMask, vtxCount);
}

// GXV_HASH=1: a hash of everything the back end is handed for a frame
// (commands, draw records, vertex / index / uniform arenas, texture uploads,
// shader sources, copies), written to the per-frame CSV. Two builds whose
// hash columns agree hand identical frames to the back end, which checks
// "pictures unchanged" for every frame of a run without rasterising any.
// GXV_HASH=2 ("content"): each draw is hashed with the vertices and indices it refers to
// instead of by position, so runs that keep geometry in different places (frame arenas / static
// buffers) agree when they draw the same things. Use with GXV_NOMERGE=1 on both sides.
static uint64_t HashBuf(const CmdBuf& b, uint64_t h, bool content) {
    auto mix = [&h](const void* p, size_t n) { if (n) h = XXH3_64bits_withSeed(p, n, h); };
    for (const Cmd& c : b.cmds) {
        if (content && (c.type == CMD_BUF_UPLOAD || c.type == CMD_BUF_FREE)) continue;
        const uint32_t v[2] = {c.type, (c.type == CMD_TEX_FREE || c.type == CMD_BUF_FREE || c.type == CMD_PROGRAM) ? c.arg : 0u};
        mix(v, sizeof v);
    }
    std::vector<uint16_t> rel;
    for (const DrawRec& d : b.draws) {
        if (content) {
            // What the draw draws, wherever the bytes live and however it addresses them:
            // its vertices, and its indices counted from its own first vertex.
            const uint32_t v[7] = {d.vsId, d.fsId, d.prim, d.texMask, d.layoutMask, d.vtxCount, d.idxCount};
            mix(v, sizeof v);
            const uint8_t* vp = b.vtx.data() + (b.vtxChunks.empty() ? 0 : b.vtxChunks[d.vchunk]);
            const uint16_t* ip = b.idx.data();
            if (d.vbuf && !StaticBufferData(d.vbuf, vp, ip)) { const uint32_t bad = 0xDEADBEEFu; mix(&bad, 4); continue; }
            const size_t stride = MakeLayout(d.layoutMask).stride;
            mix(vp + d.vtxOffset + d.vtxFirst * stride, static_cast<size_t>(d.vtxCount) * stride);
            if (d.vtxFirst) {
                rel.resize(d.idxCount);
                for (uint32_t i = 0; i < d.idxCount; ++i) rel[i] = static_cast<uint16_t>(ip[d.idxOffset + i] - d.vtxFirst);
                mix(rel.data(), static_cast<size_t>(d.idxCount) * 2);
            } else {
                mix(ip + d.idxOffset, static_cast<size_t>(d.idxCount) * 2);
            }
        } else {
            const uint32_t v[9] = {d.vsId, d.fsId, d.prim, d.texMask, d.layoutMask, d.vtxOffset, d.vtxCount, d.idxOffset, d.vbuf};
            mix(v, d.vbuf ? sizeof v : sizeof v - 4); // (unchanged hash for frames without static buffers)
            mix(&d.idxCount, 4);
            if (d.vtxFirst || d.vchunk) { const uint32_t a[2] = {d.vtxFirst, d.vchunk}; mix(a, sizeof a); } // (unchanged hash in the old addressing scheme)
        }
        mix(d.ublock, sizeof d.ublock);
        mix(&d.rs, sizeof d.rs);   // no padding (12 x u8, 4 x s16, 6 x f32)
        mix(d.tex, sizeof d.tex);  // no padding
    }
    if (!content)
        for (const BufUpload& u : b.bufUploads) {
            mix(&u.bufId, 4);
            mix(u.vtx.data(), u.vtx.size());
            mix(u.idx.data(), u.idx.size() * 2);
        }
    for (const TexUpload& u : b.uploads) {
        const uint32_t v[3] = {u.texId, u.width, u.height};
        mix(v, sizeof v);
        if (u.format != TF_RGBA8 || u.genMips) { const uint32_t f[2] = {u.format, u.genMips}; mix(f, sizeof f); } // (unchanged hash for plain RGBA8 uploads)
        for (const auto& l : u.levels) mix(l.data(), l.size());
    }
    for (const ShaderSource* sh : b.shaders) {
        const uint32_t v[2] = {sh->id, sh->fragment};
        mix(v, sizeof v);
        mix(sh->text.data(), sh->text.size());
    }
    for (const CopyTexCmd& c : b.copyTex) {
        const int32_t v[8] = {static_cast<int32_t>(c.texId), c.src.x, c.src.y, c.src.w, c.src.h, c.dstW, c.dstH, c.depth};
        mix(v, sizeof v);
        mix(c.mat, sizeof c.mat);
        mix(c.add, sizeof c.add);
        const float cl[8] = {float(c.clear.enable), float(c.clear.color), float(c.clear.alpha), float(c.clear.depth), c.clear.rgba[0],
                             c.clear.rgba[1], c.clear.rgba[2], c.clear.rgba[3]};
        mix(cl, sizeof cl);
        mix(&c.clear.z, 4);
        if (c.srcTex) { const int32_t a[7] = {static_cast<int32_t>(c.srcTex), c.srcTexW, c.srcTexH, c.dstX, c.dstY, c.dstTexW, c.dstTexH}; mix(a, sizeof a); }
        if (c.fromPass || c.fill) { // (unchanged hash for frames without isolated passes)
            const float f[6] = {float(c.fromPass), float(c.fill), c.fillRgba[0], c.fillRgba[1], c.fillRgba[2], c.fillRgba[3]};
            mix(f, sizeof f);
        }
    }
    for (const PassCmd& ps : b.passes) {
        const float v[10] = {float(ps.rect.x), float(ps.rect.y), float(ps.rect.w), float(ps.rect.h), ps.rgba[0], ps.rgba[1], ps.rgba[2], ps.rgba[3], ps.z, float(ps.planes)};
        mix(v, sizeof v);
        mix(&ps.directTex, 4);
        if (ps.atlasTex) { const uint32_t a[6] = {ps.atlasTex, ps.atlasW, ps.atlasH, ps.atlasX, ps.atlasY, ps.atlasGutter}; mix(a, sizeof a); }
    }
    for (const CopyDispCmd& c : b.copyDisp) {
        const float v[13] = {float(c.src.x), float(c.src.y), float(c.src.w), float(c.src.h), float(c.clear.enable), float(c.clear.color),
                             float(c.clear.alpha), float(c.clear.depth), c.clear.rgba[0], c.clear.rgba[1], c.clear.rgba[2], c.clear.rgba[3],
                             c.clear.z};
        mix(v, sizeof v);
        if (c.charaN) mix(c.chara, sizeof(EfbRect) * c.charaN); // (GXV_CHARAA; frames without boxes hash as before)
    }
    for (const SetTargetCmd& t : b.targets) { const uint32_t v[2] = {t.efbW, t.efbH}; mix(v, sizeof v); }
    if (!content) {
        mix(b.vtx.data(), b.vtx.size());
        mix(b.idx.data(), b.idx.size() * 2);
        if (b.vtxChunks.size() > 1) mix(b.vtxChunks.data(), b.vtxChunks.size() * 4);
    }
    mix(b.uni.data(), b.uni.size() * 4);
    mix(b.variants.data(), b.variants.size() * sizeof(VariantCmd));
    return h;
}

namespace {
ClearSpec MakeClear(bool enable) {
    ClearSpec c;
    c.enable = enable;
    if (!enable) return c;
    const uint32_t cm0 = g.bp[0x41];
    c.color = bits(cm0, 3, 1) != 0;
    c.alpha = bits(cm0, 4, 1) != 0;
    c.depth = bits(g.bp[0x40], 4, 1) != 0;
    c.rgba[0] = static_cast<float>(bits(g.bp[0x4F], 0, 8)) / 255.0f;
    c.rgba[3] = static_cast<float>(bits(g.bp[0x4F], 8, 8)) / 255.0f;
    c.rgba[1] = static_cast<float>(bits(g.bp[0x50], 8, 8)) / 255.0f;
    c.rgba[2] = static_cast<float>(bits(g.bp[0x50], 0, 8)) / 255.0f;
    if (!EfbHasAlpha()) { c.rgba[3] = 1.0f; c.alpha = true; }
    c.z = static_cast<float>(g.bp[0x51] & 0xFFFFFF) / 16777215.0f;
    return c;
}
} // namespace

void QueueCopyTex(const void* dest, bool clear) {
    CopyTexCmd c;
    c.src = g.texCopySrc;
    c.dstW = g.texCopyDstW ? g.texCopyDstW : static_cast<uint16_t>(g.texCopySrc.w);
    c.dstH = g.texCopyDstH ? g.texCopyDstH : static_cast<uint16_t>(g.texCopySrc.h);
    const uint32_t fmt = g.texCopyFmt;
    RecordCrowdCopy(c, reinterpret_cast<uintptr_t>(dest), fmt);
    stats::CopyFormat(fmt, false);
    const bool hasAlpha = EfbHasAlpha();
    // Luma weights of the hardware's RGB->Y conversion used by the intensity formats.
    const float yr = 0.257f, yg = 0.504f, yb = 0.098f, yo = 16.0f / 255.0f;
    auto row = [&](int r, float a, float b, float cc, float d) { c.mat[r][0] = a; c.mat[r][1] = b; c.mat[r][2] = cc; c.mat[r][3] = d; };
    auto ident = [&] { row(0, 1, 0, 0, 0); row(1, 0, 1, 0, 0); row(2, 0, 0, 1, 0); row(3, 0, 0, 0, 1); };
    switch (fmt) {
    case 0x00: case 0x01: // I4, I8: intensity in all four channels
        for (int r = 0; r < 4; ++r) { row(r, yr, yg, yb, 0); c.add[r] = yo; }
        break;
    case 0x02: case 0x03: // IA4, IA8
        for (int r = 0; r < 3; ++r) { row(r, yr, yg, yb, 0); c.add[r] = yo; }
        row(3, 0, 0, 0, 1);
        break;
    case 0x04: // RGB565
        ident(); row(3, 0, 0, 0, 0); c.add[3] = 1.0f;
        break;
    case 0x20: case 0x28: for (int r = 0; r < 4; ++r) row(r, 1, 0, 0, 0); break; // R4, R8
    case 0x22: case 0x23: for (int r = 0; r < 3; ++r) row(r, 1, 0, 0, 0); row(3, 0, 0, 0, 1); break; // RA4, RA8
    case 0x27: for (int r = 0; r < 4; ++r) row(r, 0, 0, 0, 1); break; // A8
    case 0x29: for (int r = 0; r < 4; ++r) row(r, 0, 1, 0, 0); break; // G8
    case 0x2A: for (int r = 0; r < 4; ++r) row(r, 0, 0, 1, 0); break; // B8
    case 0x2B: for (int r = 0; r < 3; ++r) row(r, 0, 1, 0, 0); row(3, 1, 0, 0, 0); break; // RG8 (as IA8: I=G, A=R)
    case 0x2C: for (int r = 0; r < 3; ++r) row(r, 0, 0, 1, 0); row(3, 0, 1, 0, 0); break; // GB8
    case 0x11: case 0x13: case 0x16: case 0x30: case 0x39: case 0x3A: case 0x3C:
        c.depth = true;
        ident();
        stats::Note("depth-buffer EFB copy (Z texture format) requested");
        break;
    default: // RGBA8, RGB5A3 and anything unknown
        ident();
        break;
    }
    if (!hasAlpha && !c.depth) {
        // No alpha channel in the EFB: reads return 1.
        for (int r = 0; r < 4; ++r) { c.add[r] += c.mat[r][3]; c.mat[r][3] = 0; }
    }
    c.texId = AcquireCopyTexture(dest, c.dstW, c.dstH, fmt);
    ++stats::Cur().copies;
    stats::Cur().copyPixels += static_cast<uint32_t>(c.dstW) * c.dstH;
    c.clear = MakeClear(clear);
    if (ChaOn() && c.clear.enable && c.clear.color) CharaCleared(c.src);
    if (TraceFrame())
        std::fprintf(stderr, "[trace] COPYTEX dest=%p id=%u src=(%d,%d,%d,%d) dst=%ux%u fmt=0x%02X half=%d clear=%d pe=0x%X\n", dest, c.texId, c.src.x, c.src.y,
                     c.src.w, c.src.h, c.dstW, c.dstH, fmt, g.texCopyHalf, clear, g.bp[0x43]);
    g_buf.copyTex.push_back(c);
    g_buf.cmds.push_back(Cmd{CMD_COPY_TEX, static_cast<uint32_t>(g_buf.copyTex.size() - 1)});
    g_lastDraw = -1;
    g.texDirty = true; // a bound texture may now resolve to this copy
    g.texSlotDirty = 0xFF;
    // No flush: copies are ordinary commands in the per-frame buffer.
}

void QueueCopyDisp(bool clear) {
    CopyDispCmd c;
    c.src = g.dispCopySrc;
    c.clear = MakeClear(clear);
    stats::CopyFormat(0, true);
    if (TraceFrame())
        std::fprintf(stderr, "[trace] COPYDISP src=(%d,%d,%d,%d) clear=%d color=(%.2f,%.2f,%.2f,%.2f) z=%.3f masks c%d a%d z%d\n", c.src.x, c.src.y, c.src.w,
                     c.src.h, clear, c.clear.rgba[0], c.clear.rgba[1], c.clear.rgba[2], c.clear.rgba[3], c.clear.z, c.clear.color, c.clear.alpha, c.clear.depth);
    if (ChaOn()) CharaFinish(c);
    g_buf.copyDisp.push_back(c);
    g_buf.cmds.push_back(Cmd{CMD_COPY_DISP, static_cast<uint32_t>(g_buf.copyDisp.size() - 1)});
    g_lastDraw = -1;
}

// The game copies the EFB to a texture with "clear" as its way of clearing the
// screen, several times in a row to the same scratch texture; a copy whose
// texture is copied to again (or destroyed) later in the same frame with no
// draw sampling it in between has no effect beyond its clear. On a tile-based
// GPU every copy is a render-target round trip, so those are marked to be
// skipped. (A copy that is still unused at the end of the frame is kept: the
// next frame may sample it.) GXV_DEADCOPY=0 turns this off.
static void MarkDeadCopies() {
    static const bool on = EnvInt("GXV_DEADCOPY", 1) != 0;
    if (!on || g_buf.copyTex.size() < 2) return;
    struct Live { uint32_t texId, copyIndex; };
    Live live[64];
    int n = 0;
    // (how many live entries have each low id byte: every texture of every draw is asked
    // about, and almost none is in the list)
    uint8_t inClass[256] = {};
    auto find = [&](uint32_t id) {
        if (!inClass[id & 255u]) return -1;
        for (int i = 0; i < n; ++i) if (live[i].texId == id) return i;
        return -1;
    };
    auto drop = [&](int i) { --inClass[live[i].texId & 255u]; live[i] = live[--n]; };
    for (const Cmd& c : g_buf.cmds) {
        if (c.type == CMD_DRAW) {
            if (!n) continue;
            const DrawRec& d = g_buf.draws[c.arg];
            for (int m = 0; m < 8; ++m)
                if (d.texMask & (1u << m)) {
                    const int i = find(d.tex[m].texId);
                    if (i >= 0) drop(i);
                }
        } else if (c.type == CMD_COPY_TEX) {
            const uint32_t id = g_buf.copyTex[c.arg].texId;
            const int i = find(id);
            if (i >= 0) {
                g_buf.copyTex[live[i].copyIndex].skip = true;
                ++stats::Cur().deadCopies;
                live[i].copyIndex = c.arg;
            } else if (n < 64) {
                live[n++] = Live{id, c.arg};
                ++inClass[id & 255u];
            }
        } else if (c.type == CMD_TEX_FREE) {
            const int i = find(c.arg);
            if (i >= 0) {
                g_buf.copyTex[live[i].copyIndex].skip = true;
                ++stats::Cur().deadCopies;
                drop(i);
            }
        }
    }
}

// BEGIN STATIC CROWD FRONT
bool StaticCrowdEnabled() {
    static const bool enabled = [] {
#if defined(GXV_STRICT_DRAW_PROFILE) && GXV_STRICT_DRAW_PROFILE == 1 && defined(GXV_ABSIDX_DEFAULT) && GXV_ABSIDX_DEFAULT == 0
        auto exact = [](const char* name, const char* wanted, bool absent) {
            const char* value = std::getenv(name);
            return value ? std::strcmp(value, wanted) == 0 : absent;
        };
        return exact("MSC_STATIC_CROWD", "1", false) && exact("GXV_ABSIDX", "0", true) &&
            exact("GXV_PASSISO", "1", true) && exact("GXV_BACKEND_THREAD", "0", true) &&
            exact("GXV_PLAY", "thread", true) && exact("MSC_SPLIT", "serial", false);
#else
        return false;
#endif
    }();
    return enabled;
}
namespace {
std::array<staticcrowd::Tag, staticcrowd::kMaxTags> g_crowdTags{};
uint32_t g_crowdTagCount = 0;
bool g_crowdObserved = false, g_crowdInvalid = false;
const staticcrowd::Tag* CrowdTagFor(uint64_t destination) {
    if (!g_crowdObserved || g_crowdInvalid) return nullptr;
    for (uint32_t i = 0; i < g_crowdTagCount; ++i)
        if (g_crowdTags[i].destination == destination) return &g_crowdTags[i];
    return nullptr;
}
}
void ResetCrowdTags() {
    g_crowdTagCount = 0;
    g_crowdObserved = g_crowdInvalid = false;
}
void QueueCrowdSnapshot(const staticcrowd::Tag* tags, uint32_t count, bool valid) {
    if (!StaticCrowdEnabled() || g_crowdInvalid) return;
    auto reject = [] { g_crowdInvalid = true; g_crowdTagCount = 0; };
    if (!valid || !tags || count == 0 || count > staticcrowd::kMaxTags) { reject(); return; }
    for (uint32_t i = 0; i < count; ++i) {
        if (!tags[i].valid() || tags[i].words[6] != tags[i].words[7] ||
            (tags[i].words[6] != 64 && tags[i].words[6] != 128)) { reject(); return; }
        for (uint32_t j = 0; j < i; ++j) {
            if (tags[i].destination == tags[j].destination || tags[i].words[0] == tags[j].words[0]) { reject(); return; }
        }
    }
    if (g_crowdObserved) {
        if (count != g_crowdTagCount) { reject(); return; }
        for (uint32_t i = 0; i < count; ++i) if (tags[i] != g_crowdTags[i]) { reject(); return; }
        return;
    }
    std::copy_n(tags, count, g_crowdTags.begin());
    g_crowdTagCount = count;
    g_crowdObserved = true;
}
void RecordCrowdCopy(CopyTexCmd& copy, uint64_t destination, uint32_t format) {
    if (!StaticCrowdEnabled()) return;
    copy.crowdDestination = destination;
    copy.crowdFormat = format;
    if (const auto* tag = CrowdTagFor(destination)) copy.crowd = *tag;
}
// END STATIC CROWD FRONT

// ------------------------------------------------------------ pass isolation
// The game renders many small pictures into a corner of the EFB and copies
// them to textures before it draws the scene (16 crowd impostors and 4 to 8
// shadow maps per match frame, the menus' blur chain), and it clears the
// screen by copying it somewhere "with clear". Executed literally, each copy
// makes a tile-based GPU store the whole EFB, run a copy pass and reload the
// EFB: 40 to 85 times a frame. Most of these copies do not depend on the EFB
// being one shared surface at all, and this pass over the finished frame
// finds the ones that provably do not:
//
//  - isolated pass: every draw since the previous copy has its scissor inside
//    the copy's source rectangle R; the planes (colour, alpha, depth) of R
//    that those draws or the copy read held one known constant value when the
//    draws began (they were last touched by clears with the same values); and
//    what the draws leave in R is never seen: the copy clears every plane
//    they write, or the next thing that touches R is the clear of a dead copy.
//    -> CMD_PASS_BEGIN before the draws, CopyTexCmd::fromPass.
//  - fill: the planes of R that the copy reads are known to hold a constant
//    (nothing was drawn there since they were cleared).
//    -> CopyTexCmd::fill, no source needed at all.
//
// Everything else is left as it was (efbReads in the statistics).
// What the EFB holds is tracked across frames as a short history of
// rectangles: clears with their values, and "drawn to" (unknown contents).
// The history describes the EFB as the back end keeps it, i.e. without the
// isolated passes' drawing in it.
//   GXV_PASSISO=0   leave frames as submitted (the default is on)
namespace {
// The EFB's three planes are followed separately: the menus' and cutscenes'
// blur chain works in the alpha plane alone, on top of the finished colour
// picture (alpha-only draws, alpha-only copies, alpha-only clears).
enum : uint8_t { CH_C = 1, CH_A = 2, CH_Z = 4, CH_ALL = 7 };
struct EfbEvent {
    EfbRect r;
    bool clear; // else: drawn to, contents unknown
    float v[3]; // colour: rgb; alpha, depth: v[0]
};
std::vector<EfbEvent> g_efbHist[3]; // colour, alpha, depth

// Copy textures that hold one constant colour because the last copy into them
// was a fill: the game "copies" the cleared screen corner into the same
// scratch textures every frame (up to 60 a frame), and a fill that would write
// what the texture already holds is not executed at all.
struct ConstTex {
    float rgba[4]; // as written (after the copy's conversion)
    uint16_t w, h;
};
std::unordered_map<uint32_t, ConstTex> g_constTex;

inline bool Inside(const EfbRect& a, const EfbRect& b) { // a inside b
    return a.x >= b.x && a.y >= b.y && a.x + a.w <= b.x + b.w && a.y + a.h <= b.y + b.h;
}
inline bool Overlap(const EfbRect& a, const EfbRect& b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h && a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0;
}
inline EfbRect Union(const EfbRect& a, const EfbRect& b) {
    const int x0 = std::min<int>(a.x, b.x), y0 = std::min<int>(a.y, b.y), x1 = std::max<int>(a.x + a.w, b.x + b.w), y1 = std::max<int>(a.y + a.h, b.y + b.h);
    return EfbRect{static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(x1 - x0), static_cast<int16_t>(y1 - y0)};
}
inline uint8_t ClearMask(const ClearSpec& c) {
    return c.enable ? static_cast<uint8_t>((c.color ? CH_C : 0) | (c.alpha ? CH_A : 0) | (c.depth ? CH_Z : 0)) : 0;
}
// Planes a draw changes, and planes whose previous contents its result depends on.
inline uint8_t DrawWrites(const RenderState& rs) {
    return static_cast<uint8_t>((rs.colorWrite ? CH_C : 0) | ((rs.alphaWrite || rs.dstAlphaPass) ? CH_A : 0) | ((rs.depthTest && rs.depthWrite) ? CH_Z : 0));
}
inline uint8_t DrawReads(const RenderState& rs) {
    uint8_t m = 0;
    if (rs.depthTest && rs.depthFunc != CMP_ALWAYS) m |= CH_Z;
    if (rs.blend) {
        // The colour (and, when it is written, the alpha) underneath goes into the result; the
        // alpha plane also when it is one of the blend factors.
        const bool dstAlphaFactor = rs.blendSrc == BF_DST_ALPHA || rs.blendSrc == BF_INV_DST_ALPHA || rs.blendDst == BF_DST_ALPHA || rs.blendDst == BF_INV_DST_ALPHA;
        if (rs.colorWrite) m |= CH_C;
        if (rs.alphaWrite || rs.dstAlphaPass || dstAlphaFactor) m |= CH_A;
    }
    return m;
}
inline EfbRect DrawBound(const DrawRec& d) { return EfbRect{d.exX, d.exY, d.exW, d.exH}; }
// Planes of the EFB a colour copy's result depends on.
inline uint8_t CopyReads(const CopyTexCmd& t) {
    uint8_t m = 0;
    for (int r = 0; r < 4; ++r) {
        if (t.mat[r][0] != 0 || t.mat[r][1] != 0 || t.mat[r][2] != 0) m |= CH_C;
        if (t.mat[r][3] != 0) m |= CH_A;
    }
    return m;
}

void HistPush(int plane, const EfbEvent& e) {
    if (e.r.w <= 0 || e.r.h <= 0) return;
    std::vector<EfbEvent>& h = g_efbHist[plane];
    // An older event inside the new one can never be the latest word on any pixel again.
    size_t n = 0;
    for (size_t i = 0; i < h.size(); ++i)
        if (!Inside(h[i].r, e.r)) h[n++] = h[i];
    h.resize(n);
    if (h.size() >= 48) {
        // Too fragmented to follow: everything touched so far is unknown.
        EfbEvent all = h[0];
        for (const EfbEvent& o : h) all.r = Union(all.r, o.r);
        all.clear = false;
        h.assign(1, all);
    }
    h.push_back(e);
}
void HistDrawn(uint8_t planes, const EfbRect& r) {
    for (int p = 0; p < 3; ++p) {
        if (!(planes & (1u << p))) continue;
        const std::vector<EfbEvent>& h = g_efbHist[p];
        if (!h.empty() && !h.back().clear && Inside(r, h.back().r)) continue;
        HistPush(p, EfbEvent{r, false, {}});
    }
}
void HistCleared(const EfbRect& r, const ClearSpec& c) {
    if (!c.enable) return;
    if (c.color) HistPush(0, EfbEvent{r, true, {c.rgba[0], c.rgba[1], c.rgba[2]}});
    if (c.alpha) HistPush(1, EfbEvent{r, true, {c.rgba[3], 0, 0}});
    if (c.depth) HistPush(2, EfbEvent{r, true, {c.z, 0, 0}});
}
// Does every pixel of r hold one known cleared value in each of `planes`?
// v: rgba, z (planes not asked for get 0 / z 1).
bool HistKnown(uint8_t planes, const EfbRect& r, float v[5]) {
    v[0] = v[1] = v[2] = v[3] = 0.0f;
    v[4] = 1.0f;
    for (int p = 0; p < 3; ++p) {
        if (!(planes & (1u << p))) continue;
        const std::vector<EfbEvent>& h = g_efbHist[p];
        bool have = false, covered = false;
        float val[3] = {};
        for (size_t i = h.size(); i-- > 0 && !covered;) {
            const EfbEvent& e = h[i];
            if (!Overlap(e.r, r)) continue;
            if (!e.clear) return false;
            if (have && std::memcmp(val, e.v, sizeof val) != 0) return false;
            std::memcpy(val, e.v, sizeof val);
            have = true;
            covered = Inside(r, e.r);
        }
        if (!covered) return false;
        if (p == 0) std::memcpy(v, val, sizeof val);
        else v[p == 1 ? 3 : 4] = val[0];
    }
    return true;
}

// Would this clear change nothing: does every plane it clears already hold
// exactly its value over the whole rectangle? (Most of the game's clears are
// like that once the offscreen passes no longer draw into the EFB, and each
// one that is executed is a scene of its own on the EFB's render target.)
//   GXV_NOOPCLEAR=0   execute them all the same
bool ClearIsNoOp(const EfbRect& r, const ClearSpec& c) {
    static const bool on = EnvInt("GXV_NOOPCLEAR", 1) != 0;
    if (!on || !c.enable) return false;
    const uint8_t m = ClearMask(c);
    float v[5];
    if (!HistKnown(m, r, v)) return false;
    if ((m & CH_C) && (v[0] != c.rgba[0] || v[1] != c.rgba[1] || v[2] != c.rgba[2])) return false;
    if ((m & CH_A) && v[3] != c.rgba[3]) return false;
    if ((m & CH_Z) && v[4] != c.z) return false;
    return true;
}

// After a pass's copy at command `from` that does not clear: is what the pass
// would have left in `r` of the EFB (in the planes `written`) seen by anything?
bool LeftoversUnseen(size_t from, const EfbRect& r, uint8_t written) {
    bool adjacent = true; // no draw since the copy at `from`
    for (size_t j = from + 1; j < g_buf.cmds.size(); ++j) {
        const Cmd& c = g_buf.cmds[j];
        if (c.type == CMD_DRAW) {
            const DrawRec& d = g_buf.draws[c.arg];
            const EfbRect b = DrawBound(d);
            if (b.w <= 0 || b.h <= 0) continue;
            adjacent = false;
            if (!Overlap(b, r)) continue;
            // A rectangle drawn over all of it, whatever was there, in every plane the pass wrote:
            // the leftovers are gone before anything looked at them. (The game ends its bloom
            // chain, which works in the corners of the EFB, by putting the saved picture back.)
            if (d.cover && Inside(r, b) && (written & ~DrawWrites(d.rs)) == 0 && (DrawReads(d.rs) & written) == 0) return true;
            return false;
        } else if (c.type == CMD_COPY_TEX) {
            const CopyTexCmd& t = g_buf.copyTex[c.arg];
            if (!Overlap(t.src, r)) continue;
            // Another copy of the same rectangle straight after: it is made from the pass too
            // (IsolatePasses), not from the EFB.
            if (adjacent && !t.depth && !t.skip && !t.clear.enable && t.src.x == r.x && t.src.y == r.y && t.src.w == r.w && t.src.h == r.h) continue;
            if (!t.skip) return false;
            if ((written & ~ClearMask(t.clear)) == 0 && Inside(r, t.src)) return true;
        } else if (c.type == CMD_COPY_DISP) {
            if (Overlap(g_buf.copyDisp[c.arg].src, r)) return false;
        } else if (c.type == CMD_SET_TARGET) {
            return false;
        }
    }
    return false;
}

void IsolatePasses() {
    static const bool on = EnvInt("GXV_PASSISO", 1) != 0;
    if (!on) return;
    stats::Frame& fr = stats::Cur();
    std::vector<Cmd>& cmds = g_buf.cmds;
    size_t runFirst = 0; // index in cmds of the first draw of the current run
    uint32_t runDraws = 0;
    uint8_t runWrites = 0, runReads = 0;
    EfbRect runBox{};
    // The run's first draw when it is a covering rectangle: the planes it writes, over its bound,
    // do not depend on what the EFB held (see DrawRec::cover).
    uint8_t runCoverPlanes = 0;
    EfbRect runCoverBox{};
    // The last isolated pass, while no draw has followed its copy: a further copy of the same
    // rectangle is made from the same pass (the game copies one blurred picture at two sizes).
    int chainCopy = -1, chainPass = -1;
    EfbRect chainSrc{};
    std::vector<std::pair<size_t, uint32_t>> begins; // (index of the run's first draw, pass index)
    auto runToEfb = [&] {
        if (runDraws) HistDrawn(runWrites, runBox);
        runDraws = 0;
        runWrites = runReads = 0;
    };
    static const long passLog = diag::Int("GXV_PASSLOG", -1); // <frame>: why each copy of that frame was or was not isolated
    const bool logThis = passLog >= 0 && static_cast<uint64_t>(passLog) == g_frame;
    for (size_t i = 0; i < cmds.size(); ++i) {
        const Cmd c = cmds[i];
        switch (c.type) {
        case CMD_DRAW: {
            const DrawRec& dr = g_buf.draws[c.arg];
            const RenderState& rs = dr.rs;
            const EfbRect sc = DrawBound(dr);
            if (sc.w <= 0 || sc.h <= 0) break; // draws nothing
            chainCopy = -1;
            if (!runDraws) {
                runFirst = i;
                runBox = sc;
                runCoverPlanes = dr.cover ? DrawWrites(rs) : 0;
                runCoverBox = sc;
                // (what the cover itself reads does not count: it reads nothing it does not overwrite)
                runWrites |= DrawWrites(rs);
                if (!dr.cover) runReads |= DrawReads(rs);
                ++runDraws;
                break;
            }
            runBox = Union(runBox, sc);
            runWrites |= DrawWrites(rs);
            runReads |= DrawReads(rs);
            ++runDraws;
            break;
        }
        case CMD_COPY_TEX: {
            CopyTexCmd& t = g_buf.copyTex[c.arg];
            const auto* currentCrowd = CrowdTagFor(t.crowdDestination);
            if (!currentCrowd || t.crowd != *currentCrowd) t.crowd = {};
            const uint8_t copyReads = t.depth ? CH_Z : CopyReads(t);
            float v[5];
            // A second copy of the rectangle the last isolated pass was copied from, with no draw
            // in between: from that pass as well. (The pass then keeps its own colour buffer.)
            if (!runDraws && chainCopy >= 0 && !t.skip && !t.depth && !t.clear.enable && t.src.x == chainSrc.x && t.src.y == chainSrc.y &&
                t.src.w == chainSrc.w && t.src.h == chainSrc.h) {
                g_buf.copyTex[chainCopy].passMore = true;
                if (g_buf.passes[chainPass].directTex) {
                    g_buf.passes[chainPass].directTex = 0;
                    g_buf.passes[chainPass].crowd = {};
                    --fr.passesDirect;
                }
                t.fromPass = true;
                g_constTex.erase(t.texId);
                chainCopy = static_cast<int>(c.arg);
                if (logThis) std::fprintf(stderr, "[passlog] copy -> tex %u: a further copy of the same pass\n", t.texId);
                break;
            }
            chainCopy = -1;
            // The pass target has to reproduce every plane the draws or the copy read; planes the
            // draws only write and nobody reads do not matter, and neither do planes a covering
            // first draw writes over the whole rectangle. What the draws would have left in
            // the EFB must be wiped by this copy's clear, or (no clear) never be looked at.
            const uint8_t covered = (runDraws && Inside(t.src, runCoverBox)) ? runCoverPlanes : 0;
            const uint8_t need = static_cast<uint8_t>((runReads | copyReads) & ~covered);
            const bool inside = runDraws && Inside(runBox, t.src);
            const bool known = inside && HistKnown(need, t.src, v);
            const bool unseen = known && (t.clear.enable ? (runWrites & ~ClearMask(t.clear)) == 0 : LeftoversUnseen(i, t.src, runWrites));
            if (logThis)
                std::fprintf(stderr, "[passlog] copy -> tex %u src (%d,%d)-(%d,%d): %u draws bound (%d,%d)-(%d,%d) writes %u reads %u copy reads %u cover %u -> %s\n", t.texId,
                             t.src.x, t.src.y, t.src.x + t.src.w, t.src.y + t.src.h, runDraws, runBox.x, runBox.y, runBox.x + runBox.w, runBox.y + runBox.h, runWrites,
                             runReads, copyReads, covered,
                             !runDraws ? "no draws" : (t.skip || t.depth) ? "skip/depth" : !inside ? "draws outside the rectangle" : !known ? "contents before the draws not known" :
                             !unseen ? "leftovers seen later" : "ISOLATED");
            if (runDraws && !t.skip && !t.depth && t.src.w > 0 && t.src.h > 0 && inside && known && unseen) {
                PassCmd ps;
                ps.rect = t.src;
                std::memcpy(ps.rgba, v, sizeof ps.rgba);
                ps.z = v[4];
                ps.planes = static_cast<uint8_t>(runReads | copyReads | runWrites);
                ps.known = need;
                // Same size, no conversion, and the pass does not sample its own destination:
                // it can be drawn straight into the destination texture.
                bool direct = t.dstW == t.src.w && t.dstH == t.src.h && t.add[0] == 0 && t.add[1] == 0 && t.add[2] == 0 && t.add[3] == 0;
                for (int r = 0; r < 4 && direct; ++r)
                    for (int col = 0; col < 4; ++col)
                        if (t.mat[r][col] != (r == col ? 1.0f : 0.0f)) direct = false;
                for (size_t j = runFirst; j < i && direct; ++j) {
                    if (cmds[j].type != CMD_DRAW) continue;
                    const DrawRec& d = g_buf.draws[cmds[j].arg];
                    for (int m = 0; m < 8; ++m)
                        if ((d.texMask & (1u << m)) && d.tex[m].texId == t.texId) direct = false;
                }
                if (direct) {
                    ps.directTex = t.texId;
                    ++fr.passesDirect;
                    if (t.crowd.valid() && t.crowdFormat == 5 && t.dstW == t.crowd.words[6] && t.dstH == t.crowd.words[7])
                        ps.crowd = t.crowd;
                }
                g_constTex.erase(t.texId);
                g_buf.passes.push_back(ps);
                begins.emplace_back(runFirst, static_cast<uint32_t>(g_buf.passes.size() - 1));
                t.fromPass = true;
                ++fr.passes;
                fr.passDraws += runDraws;
                runDraws = 0;
                runWrites = runReads = 0;
                if (!t.clear.enable) {
                    chainCopy = static_cast<int>(c.arg);
                    chainPass = static_cast<int>(g_buf.passes.size()) - 1;
                    chainSrc = t.src;
                }
            } else {
                runToEfb();
                if (!t.skip && !t.depth && HistKnown(copyReads, t.src, v)) {
                    t.fill = true;
                    std::memcpy(t.fillRgba, v, sizeof t.fillRgba);
                    ++fr.fills;
                    // The colour the back end will write (same arithmetic as its fill).
                    ConstTex ct;
                    ct.w = t.dstW;
                    ct.h = t.dstH;
                    float q[4];
                    for (int k = 0; k < 4; ++k) {
                        const float cv = v[k] < 0 ? 0 : (v[k] > 1 ? 1 : v[k]);
                        q[k] = std::floor(cv * 255.0f + 0.5f) / 255.0f;
                    }
                    for (int r = 0; r < 4; ++r) ct.rgba[r] = t.mat[r][0] * q[0] + t.mat[r][1] * q[1] + t.mat[r][2] * q[2] + t.mat[r][3] * q[3] + t.add[r];
                    auto it = g_constTex.find(t.texId);
                    if (it != g_constTex.end() && it->second.w == ct.w && it->second.h == ct.h && std::memcmp(it->second.rgba, ct.rgba, sizeof ct.rgba) == 0) {
                        t.skip = true; // the texture already holds exactly this
                        ++fr.fillsRedundant;
                    } else {
                        g_constTex[t.texId] = ct;
                    }
                } else if (!t.skip) {
                    ++fr.efbReads;
                    g_constTex.erase(t.texId);
                }
            }
            if (ClearIsNoOp(t.src, t.clear)) {
                t.clear.enable = false;
                t.clearElided = true;
                ++fr.clearsRedundant;
            } else {
                HistCleared(t.src, t.clear);
            }
            break;
        }
        case CMD_COPY_DISP: {
            runToEfb();
            CopyDispCmd& d = g_buf.copyDisp[c.arg];
            if (ClearIsNoOp(d.src, d.clear)) {
                d.clear.enable = false;
                d.clearElided = true;
                ++fr.clearsRedundant;
            } else {
                HistCleared(d.src, d.clear);
            }
            break;
        }
        case CMD_SET_TARGET:
            runToEfb();
            for (auto& h : g_efbHist) h.clear();
            g_constTex.clear(); // (the back end's copy textures change size with the target)
            break;
        case CMD_TEX_FREE:
            g_constTex.erase(c.arg);
            break;
        case CMD_PRESENT:
            runToEfb();
            break;
        default:
            break;
        }
    }
    if (begins.empty()) return;
    // Insert the CMD_PASS_BEGIN commands (back to front, so the recorded indices stay valid).
    for (size_t k = begins.size(); k-- > 0;) cmds.insert(cmds.begin() + static_cast<std::ptrdiff_t>(begins[k].first), Cmd{CMD_PASS_BEGIN, begins[k].second});
}
} // namespace

// ------------------------------------------------------------- copy atlas
// After pass isolation an open-play frame still has 16 offscreen passes that
// each draw one crowd figure into a 64x64 texture of its own, and 4 to 8 that
// draw a player's shadow at 144x144 and copy it into a 72x72 texture (and more
// of both kinds in cutscenes): 24 to 32 render-target switches, each a scene
// of its own for the console's GPU. The pictures are then sampled by plain
// quads. This step gives such pictures cells of shared atlas textures:
//  - a direct pass (PassCmd::directTex) of at most kAtlasMaxCell pixels a side
//    is drawn into a cell of the atlas for its size (PassCmd::atlasTex ...);
//  - a pass whose copy converts or scales (the shadows) is drawn into a cell
//    of a scratch atlas, and its copy becomes a copy from that cell into a
//    cell of the picture atlas for the copy's size. The copies of passes that
//    follow each other are held back until the last of them has been drawn,
//    so that the passes are one scene and the copies another;
//  - both only when the frame has at least two such passes of one size, or
//    the picture lives in an atlas already;
//  - every later draw that samples the picture is redirected: its texture
//    binding becomes the atlas and its texture coordinates are rewritten in
//    the vertex arena to address the cell. That is only possible for draws
//    whose coordinates for that texture are known here: vertices in the
//    frame's arena, one plain texture-coordinate attribute through an identity
//    texture matrix, every coordinate inside 0..1 (so that clamping never
//    comes into play). Draws that only differed in which picture they sampled
//    become mergeable and are merged;
//  - a draw that samples the picture in any other way takes it back out of
//    the atlas first (a copy from the cell into the picture's own texture),
//    and that picture stays out for a while (kAtlasBackOff frames, doubling each time).
// Where a picture lives is remembered across frames (a picture that is not
// drawn again in a frame keeps its cell and its contents).
// Not identical to separate textures in one respect: at the very edge of a
// picture, linear filtering blends with the cleared gutter instead of
// repeating the edge texel. For pictures whose border is the clear colour
// (figures on a transparent ground) that is the same thing.
//   GXV_ATLAS=0   off;  =1  direct passes only;  2 (default)  converted copies too
//   (needs absolute indices, GXV_ABSIDX, for the merging)
namespace {
constexpr int kAtlasGutter = 1;
constexpr int kAtlasMaxCell = 128;   // largest picture
constexpr int kAtlasMaxSource = 288; // largest pass of a converted copy
constexpr int kAtlasMaxRatio = 4;    // pass size : picture size of a converted copy
struct AtlasPage {
    uint32_t tex = 0;              // back-end texture id
    uint16_t cellW = 0, cellH = 0; // size of a picture
    uint16_t gutter = kAtlasGutter;
    uint16_t cols = 0, rows = 0;
    std::vector<uint32_t> owner;   // texture id of the picture in each cell, 0 = free
    std::vector<uint64_t> lastUse; // frame in which the cell's picture was last drawn or sampled
    struct Gut { float rgba[4]; bool valid; };
    std::vector<Gut> gut;          // what the cell's gutter was last cleared to
    uint16_t PitchX() const { return static_cast<uint16_t>(cellW + 2 * gutter); }
    uint16_t PitchY() const { return static_cast<uint16_t>(cellH + 2 * gutter); }
    uint16_t W() const { return static_cast<uint16_t>(cols * PitchX()); }
    uint16_t H() const { return static_cast<uint16_t>(rows * PitchY()); }
    uint16_t CellX(uint32_t c) const { return static_cast<uint16_t>((c % cols) * PitchX() + gutter); }
    uint16_t CellY(uint32_t c) const { return static_cast<uint16_t>((c / cols) * PitchY() + gutter); }
    void Init(uint16_t w, uint16_t h, uint16_t g, int side) {
        tex = AllocTextureId();
        cellW = w;
        cellH = h;
        gutter = g;
        cols = static_cast<uint16_t>(std::max(1, side / (w + 2 * g)));
        rows = static_cast<uint16_t>(std::max(1, side / (h + 2 * g)));
        owner.assign(static_cast<size_t>(cols) * rows, 0);
        lastUse.assign(owner.size(), 0);
        gut.assign(owner.size(), Gut{{0, 0, 0, 0}, false});
    }
    // Fills in the atlas part of a pass that draws into cell c. The gutter is only cleared
    // when it does not hold the pass's clear colour already (one scissor rectangle per
    // cell instead of two for the back end).
    void Target(PassCmd& ps, uint32_t c) {
        ps.atlasTex = tex;
        ps.atlasW = W();
        ps.atlasH = H();
        ps.atlasX = CellX(c);
        ps.atlasY = CellY(c);
        Gut& g = gut[c];
        if (g.valid && std::memcmp(g.rgba, ps.rgba, sizeof g.rgba) == 0) {
            ps.atlasGutter = 0;
        } else {
            ps.atlasGutter = static_cast<uint8_t>(gutter);
            std::memcpy(g.rgba, ps.rgba, sizeof g.rgba);
            g.valid = true;
        }
    }
};
struct AtlasLoc { uint16_t page, cell; };
std::vector<AtlasPage> g_atlasPages;               // pictures
std::vector<AtlasPage> g_scratchPages;             // passes of converted copies (cells are free again once the copies are made)
// A map from texture id to V as an array (ids are small and dense): find() is looked up for
// every texture of every draw, and a hash table costs a division each time on the console's
// processor. Only what the code below uses; find() gives a pointer (null = end()) that stays
// valid until the next operator[].
template <typename V> struct IdMap {
    struct Slot { uint32_t first; V second; bool used; };
    std::vector<Slot> s;
    size_t n = 0;
    Slot* find(uint32_t id) { return id < s.size() && s[id].used ? &s[id] : nullptr; }
    Slot* end() const { return nullptr; }
    size_t count(uint32_t id) const { return id < s.size() && s[id].used ? 1 : 0; }
    V& operator[](uint32_t id) {
        if (id >= s.size()) s.resize(static_cast<size_t>(id) + 256, Slot{0, V{}, false});
        Slot& x = s[id];
        if (!x.used) { x.used = true; x.first = id; x.second = V{}; ++n; }
        return x.second;
    }
    void erase(Slot* it) { if (it && it->used) { it->used = false; --n; } }
    void erase(uint32_t id) { erase(find(id)); }
    bool empty() const { return n == 0; }
    void clear() { for (Slot& x : s) x.used = false; n = 0; }
};
IdMap<AtlasLoc> g_atlasOf;  // picture (texture id) -> where it lives
// Pictures that had to be taken out of an atlas stay out for a while, twice as long each time.
struct AtlasBar { uint64_t until; uint8_t level; };
IdMap<AtlasBar> g_atlasBar;
constexpr uint64_t kAtlasBackOff = 300; // frames, the first time
bool AtlasBarred(uint32_t texId) {
    if (g_atlasBar.empty()) return false;
    auto it = g_atlasBar.find(texId);
    return it != g_atlasBar.end() && g_frame < it->second.until;
}
void AtlasBarNow(uint32_t texId) {
    AtlasBar& b = g_atlasBar[texId];
    b.until = g_frame + (kAtlasBackOff << b.level);
    if (b.level < 12) ++b.level;
}
void AtlasRelease(uint32_t texId) {
    auto it = g_atlasOf.find(texId);
    if (it == g_atlasOf.end()) return;
    g_atlasPages[it->second.page].owner[it->second.cell] = 0;
    g_atlasOf.erase(it);
}
// Appends a copy of cell `loc`'s picture into its own texture `texId` to `out` (the picture
// leaves the atlas with its contents).
void AtlasExtract(std::vector<Cmd>& out, uint32_t texId, AtlasLoc loc) {
    const AtlasPage& pg = g_atlasPages[loc.page];
    CopyTexCmd x;
    x.texId = texId;
    x.srcTex = pg.tex;
    x.srcTexW = pg.W();
    x.srcTexH = pg.H();
    x.src = EfbRect{static_cast<int16_t>(pg.CellX(loc.cell)), static_cast<int16_t>(pg.CellY(loc.cell)), static_cast<int16_t>(pg.cellW),
                    static_cast<int16_t>(pg.cellH)};
    x.dstW = pg.cellW;
    x.dstH = pg.cellH;
    for (int r = 0; r < 4; ++r) x.mat[r][r] = 1.0f;
    g_buf.copyTex.push_back(x);
    out.push_back(Cmd{CMD_COPY_TEX, static_cast<uint32_t>(g_buf.copyTex.size() - 1)});
    AtlasRelease(texId);
    ++stats::Cur().atlasEvicted;
}
// A cell for a picture of w x h (its own if it has one). All pictures of one size share one
// atlas as long as it has room, so that their passes stay one scene and the draws that
// sample them can merge; when it is full, the picture that has gone unused the longest (at
// least kAtlasIdle frames) gets a texture of its own again (`out`: where that copy goes).
constexpr uint64_t kAtlasIdle = 120;
AtlasLoc AtlasCellFor(std::vector<Cmd>& out, uint32_t texId, uint16_t w, uint16_t h) {
    auto it = g_atlasOf.find(texId);
    if (it != g_atlasOf.end()) {
        const AtlasPage& pg = g_atlasPages[it->second.page];
        if (pg.cellW == w && pg.cellH == h) return it->second;
        AtlasRelease(texId);
    }
    for (int reclaim = 0; reclaim < 2; ++reclaim)
        for (size_t p = 0; p < g_atlasPages.size(); ++p) {
            AtlasPage& pg = g_atlasPages[p];
            if (pg.cellW != w || pg.cellH != h) continue;
            int best = -1;
            for (size_t c = 0; c < pg.owner.size(); ++c) {
                if (!reclaim) {
                    if (!pg.owner[c]) { best = static_cast<int>(c); break; }
                } else if (pg.owner[c] && pg.lastUse[c] + kAtlasIdle < g_frame && (best < 0 || pg.lastUse[c] < pg.lastUse[best])) {
                    best = static_cast<int>(c);
                }
            }
            if (best < 0) continue;
            const AtlasLoc loc{static_cast<uint16_t>(p), static_cast<uint16_t>(best)};
            if (pg.owner[best]) {
                // (only if the old owner really lives here: a pass that is still open has the cell but no entry yet)
                auto o = g_atlasOf.find(pg.owner[best]);
                if (o == g_atlasOf.end() || o->second.page != loc.page || o->second.cell != loc.cell) continue;
                AtlasExtract(out, pg.owner[best], loc);
            }
            pg.owner[best] = texId;
            pg.lastUse[best] = g_frame;
            return loc;
        }
    AtlasPage pg;
    // About 512 pixels a side (6 x 6 pictures of 72, 3 x 3 of 128), but room for 100 of the
    // smallest: the walk-out and the replays draw 64 crowd pictures of 64 x 64 per frame, and
    // pictures spread over two pages cost a scene per change of page and keep the draws that
    // sample them from merging.
    // GXV_ATLAS_SIDE=<pixels> sets the side for the smallest pictures (264: 16 of them,
    // 330: 25, 462: 49). On the console a scissor change costs a pass over the whole target
    // (vitaGL keeps the scissor as a mask bit per pixel), so smaller pages trade scenes
    // against that; nothing here can tell which is cheaper.
    static const int smallSide = static_cast<int>(EnvInt("GXV_ATLAS_SIDE", 672));
    pg.Init(w, h, kAtlasGutter, (w <= 64 && h <= 64) ? smallSide : 512);
    pg.owner[0] = texId;
    pg.lastUse[0] = g_frame;
    g_atlasPages.push_back(pg);
    return AtlasLoc{static_cast<uint16_t>(g_atlasPages.size() - 1), 0};
}

// Points draw `d`'s sampling of texmap `m` (a picture in cell `loc`) at the atlas by
// rewriting the draw's texture coordinates. False (nothing changed) when that cannot be done.
// commit = false: only says whether it can be done.
int g_atlasWhy = 0; // which test of AtlasRedirect failed last (diagnostics)
inline bool Why(int n) { g_atlasWhy = n; return false; }
bool AtlasRedirect(DrawRec& d, int m, AtlasLoc loc, bool commit) {
    if (d.vbuf) return Why(1); // static geometry: the coordinates are not ours to change
    const FsKey& fs = g_fsReg.keys[d.fsId];
    const VsKey& vs = g_vsReg.keys[d.vsId];
    if (fs.numIndStages) return Why(2);
    int coord = -1;
    for (int s = 0; s < fs.numStages; ++s) {
        const FsKey::Stage& st = fs.stage[s];
        if (!st.texEnable || st.texMap != m) continue;
        if (coord >= 0 && st.texCoord != coord) return Why(3);
        coord = st.texCoord;
    }
    if (coord < 0 || coord >= vs.numTexGens) return Why(4);
    for (int s = 0; s < fs.numStages; ++s) // the same coordinate must not address another texture as well
        if (fs.stage[s].texEnable && fs.stage[s].texMap != m && fs.stage[s].texCoord == coord) return Why(5);
    const VsKey::TexGen& tg = vs.tg[coord];
    if (tg.type != 0 || tg.src < 5 || tg.src > 12 || tg.post || tg.perVtxMtx) return Why(6);
    // (a projective coordinate is fine when its third row makes q = 1: the game draws the same quads both ways)
    const bool proj = tg.proj || (fs.texProjMask & (1u << coord));
    if (proj && !(tg.proj && (fs.texProjMask & (1u << coord)))) return Why(11);
    for (int c = 0; c < vs.numTexGens; ++c) // nor may the attribute feed another coordinate
        if (c != coord && vs.tg[c].type < 2 && vs.tg[c].src == tg.src) return Why(7);
    const int k = tg.src - 5;
    if (!(d.layoutMask & (VL_TEX0 << k)) || d.ublock[UB_TEXM] == kNoBlock) return Why(8);
    static const float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    if (std::memcmp(g_buf.uni.data() + d.ublock[UB_TEXM] + coord * 12, ident, proj ? 48 : 32) != 0) return Why(9);
    const VtxLayout L = MakeLayout(d.layoutMask);
    uint8_t* const v0 = g_buf.vtx.data() + (g_buf.vtxChunks.empty() ? 0 : g_buf.vtxChunks[d.vchunk]) + d.vtxOffset +
                        static_cast<size_t>(d.vtxFirst) * L.stride + L.offTex[k];
    // Every coordinate inside 0..1. On the bit patterns: a float is in [+0, 1] exactly when
    // its bits, read as an unsigned number, are at most those of 1.0; -0 also counts; a NaN
    // or anything negative does not. (Four floating-point comparisons per vertex cost four
    // transfers of the status word, some 9 000 vertices a frame.)
    {
        const uint8_t* q = v0;
        const size_t st = L.stride;
        uint32_t bad = 0;
        for (uint32_t i = 0; i < d.vtxCount; ++i, q += st) {
            uint32_t b[2];
            std::memcpy(b, q, 8);
            bad |= static_cast<uint32_t>(b[0] > 0x3F800000u && b[0] != 0x80000000u) | static_cast<uint32_t>(b[1] > 0x3F800000u && b[1] != 0x80000000u);
        }
        if (bad) return Why(10);
    }
    if (!commit) return true;
    const AtlasPage& pg = g_atlasPages[loc.page];
    const float sx = static_cast<float>(pg.cellW) / pg.W(), sy = static_cast<float>(pg.cellH) / pg.H();
    const float ox = static_cast<float>(pg.CellX(loc.cell)) / pg.W(), oy = static_cast<float>(pg.CellY(loc.cell)) / pg.H();
    {
        uint8_t* q = v0;
        const size_t st = L.stride;
        for (uint32_t i = 0; i < d.vtxCount; ++i, q += st) {
            float uv[2];
            std::memcpy(uv, q, 8);
            uv[0] = ox + uv[0] * sx;
            uv[1] = oy + uv[1] * sy;
            std::memcpy(q, uv, 8);
        }
    }
    d.tex[m].texId = pg.tex;
    return true;
}

// GXV_PASSREPEAT=1 (diagnostic): how often an offscreen pass draws exactly what the pass
// into the same texture drew the frame before, or what another pass of the same frame draws
// (candidates for not being drawn at all). The hash covers the pass's draws with their
// state, uniform values and geometry; it does not know when a sampled texture's contents
// change under the same id, so the counts are an upper bound.
struct PassRepeat {
    std::unordered_map<uint32_t, uint64_t> last; // destination texture -> hash of its last pass
    uint64_t passes = 0, sameAsLastFrame = 0, sameAsOther = 0, frames = 0;
} g_passRepeat;
void MeasurePassRepeats() {
    static const bool on = diag::Int("GXV_PASSREPEAT", 0) != 0;
    if (!on || g_buf.passes.empty()) return;
    ++g_passRepeat.frames;
    std::vector<uint64_t> seen;
    uint64_t h = 0;
    bool open = false;
    for (const Cmd& c : g_buf.cmds) {
        if (c.type == CMD_PASS_BEGIN) {
            const PassCmd& ps = g_buf.passes[c.arg];
            h = XXH3_64bits(&ps.rect, sizeof ps.rect) ^ XXH3_64bits(ps.rgba, sizeof ps.rgba);
            open = true;
        } else if (c.type == CMD_DRAW && open) {
            const DrawRec& d = g_buf.draws[c.arg];
            const uint32_t v[8] = {d.vsId, d.fsId, d.prim, d.texMask, d.layoutMask, d.vtxCount, d.idxCount, d.vbuf};
            h = XXH3_64bits_withSeed(v, sizeof v, h);
            h = XXH3_64bits_withSeed(&d.rs, sizeof d.rs, h);
            h = XXH3_64bits_withSeed(d.tex, sizeof d.tex, h);
            for (int b = 0; b < UB_COUNT; ++b)
                if (d.ublock[b] != kNoBlock) // (the block may be shorter than kUbSize: an upper bound that stays inside the arena)
                    h = XXH3_64bits_withSeed(g_buf.uni.data() + d.ublock[b], std::min<size_t>(kUbSize[b], g_buf.uni.size() - d.ublock[b]) * 4, h);
            if (d.vbuf) {
                const uint32_t g2[3] = {d.vtxOffset, d.vtxFirst, d.idxOffset};
                h = XXH3_64bits_withSeed(g2, sizeof g2, h);
            } else {
                const size_t stride = MakeLayout(d.layoutMask).stride;
                const uint8_t* vp = g_buf.vtx.data() + (g_buf.vtxChunks.empty() ? 0 : g_buf.vtxChunks[d.vchunk]) + d.vtxOffset + d.vtxFirst * stride;
                h = XXH3_64bits_withSeed(vp, d.vtxCount * stride, h);
            }
        } else if (c.type == CMD_COPY_TEX && open) {
            open = false;
            const uint32_t t = g_buf.copyTex[c.arg].texId;
            ++g_passRepeat.passes;
            auto it = g_passRepeat.last.find(t);
            if (it != g_passRepeat.last.end() && it->second == h) ++g_passRepeat.sameAsLastFrame;
            else if (std::find(seen.begin(), seen.end(), h) != seen.end()) ++g_passRepeat.sameAsOther;
            seen.push_back(h);
            g_passRepeat.last[t] = h;
        }
    }
}

void AtlasPasses() {
    MeasurePassRepeats();
    static const long mode = AbsoluteIndices() ? EnvInt("GXV_ATLAS", 2) : 0;
    if (!mode || (g_buf.passes.empty() && g_atlasOf.empty())) return;
    stats::Frame& fr = stats::Cur();
    std::vector<Cmd>& cmds = g_buf.cmds;
    // The copy each pass ends with, and how many passes of each kind and size the frame has.
    static std::vector<uint32_t> passCopy;
    passCopy.assign(g_buf.passes.size(), 0xFFFFFFFFu);
    struct Kind { uint16_t sw, sh, dw, dh; bool direct; uint32_t n; };
    Kind kinds[8];
    int nKinds = 0;
    // Is this pass one for an atlas at all? (k: its kind)
    auto classify = [&](const PassCmd& ps, uint32_t copy, Kind& k) {
        if (copy == 0xFFFFFFFFu || ps.rect.w <= 0 || ps.rect.h <= 0) return false;
        const CopyTexCmd& t = g_buf.copyTex[copy];
        if (!t.fromPass || t.skip || t.depth || t.fill || t.srcTex || t.passMore) return false; // (passMore: copied more than once, keeps its own buffer)
        k = Kind{static_cast<uint16_t>(ps.rect.w), static_cast<uint16_t>(ps.rect.h), t.dstW, t.dstH, ps.directTex != 0, 0};
        if (k.dw == 0 || k.dh == 0 || k.dw > kAtlasMaxCell || k.dh > kAtlasMaxCell) return false;
        if (k.direct) return ps.directTex == t.texId && k.sw == k.dw && k.sh == k.dh;
        if (mode < 2 || k.sw > kAtlasMaxSource || k.sh > kAtlasMaxSource) return false;
        if (t.src.x != ps.rect.x || t.src.y != ps.rect.y || t.src.w != ps.rect.w || t.src.h != ps.rect.h) return false;
        return k.sw % k.dw == 0 && k.sh % k.dh == 0 && k.sw / k.dw == k.sh / k.dh && k.sw / k.dw <= kAtlasMaxRatio;
    };
    auto sameKind = [](const Kind& a, const Kind& b) { return a.sw == b.sw && a.sh == b.sh && a.dw == b.dw && a.dh == b.dh && a.direct == b.direct; };
    {
        uint32_t open = 0xFFFFFFFFu;
        for (const Cmd& c : cmds) {
            if (c.type == CMD_PASS_BEGIN) open = c.arg;
            else if (c.type == CMD_COPY_TEX) {
                if (open != 0xFFFFFFFFu) {
                    passCopy[open] = c.arg;
                    Kind k;
                    if (classify(g_buf.passes[open], c.arg, k)) {
                        int i = 0;
                        while (i < nKinds && !sameKind(kinds[i], k)) ++i;
                        if (i == nKinds && nKinds < 8) kinds[nKinds++] = k;
                        if (i < nKinds) ++kinds[i].n;
                    }
                }
                open = 0xFFFFFFFFu;
            }
        }
    }
    static std::vector<Cmd> out;
    out.clear();
    out.reserve(cmds.size() + 16);
    // The pass that is open (between its CMD_PASS_BEGIN and its copy), if it draws into an atlas.
    uint32_t openTex = 0;      // the picture
    uint32_t openCopy = 0;     // its copy command
    AtlasLoc openLoc{};        // the picture's cell
    bool openDirect = false;
    uint16_t openScratchCell = 0;
    bool inPass = false;       // inside any pass (atlas or not)
    // Converted copies that wait for the passes after them (all from one scratch atlas).
    struct Blit { uint32_t texId, copy; AtlasLoc dst; uint16_t cell; };
    static std::vector<Blit> pending;
    pending.clear();
    int scratch = -1; // index in g_scratchPages of the batch's atlas
    auto flush = [&] {
        if (pending.empty()) return;
        const AtlasPage& sp = g_scratchPages[scratch];
        for (const Blit& b : pending) {
            const AtlasPage& dp = g_atlasPages[b.dst.page];
            const CopyTexCmd& orig = g_buf.copyTex[b.copy];
            CopyTexCmd x;
            // From the pass's cell with its gutter into the picture's cell with its gutter: the
            // same mapping of texels as the original copy, and the picture's gutter gets what
            // the copy makes of the pass's clear colour.
            x.texId = dp.tex;
            x.dstTexW = dp.W();
            x.dstTexH = dp.H();
            x.dstX = static_cast<int16_t>(dp.CellX(b.dst.cell) - dp.gutter);
            x.dstY = static_cast<int16_t>(dp.CellY(b.dst.cell) - dp.gutter);
            x.dstW = static_cast<uint16_t>(dp.cellW + 2 * dp.gutter);
            x.dstH = static_cast<uint16_t>(dp.cellH + 2 * dp.gutter);
            x.srcTex = sp.tex;
            x.srcTexW = sp.W();
            x.srcTexH = sp.H();
            x.src = EfbRect{static_cast<int16_t>(sp.CellX(b.cell) - sp.gutter), static_cast<int16_t>(sp.CellY(b.cell) - sp.gutter),
                            static_cast<int16_t>(sp.cellW + 2 * sp.gutter), static_cast<int16_t>(sp.cellH + 2 * sp.gutter)};
            std::memcpy(x.mat, orig.mat, sizeof x.mat);
            std::memcpy(x.add, orig.add, sizeof x.add);
            g_buf.copyTex.push_back(x);
            out.push_back(Cmd{CMD_COPY_TEX, static_cast<uint32_t>(g_buf.copyTex.size() - 1)});
            g_atlasOf[b.texId] = b.dst; // the picture now lives in its cell
            g_atlasPages[b.dst.page].gut[b.dst.cell].valid = false; // (written by a copy, not by a pass's clear)
        }
        pending.clear();
        scratch = -1;
    };
    auto isPending = [&](uint32_t texId) {
        for (const Blit& b : pending)
            if (b.texId == texId) return true;
        return false;
    };
    for (size_t i = 0; i < cmds.size(); ++i) {
        const Cmd c = cmds[i];
        switch (c.type) {
        case CMD_PASS_BEGIN: {
            // Nothing can be taken out of an atlas in the middle of a pass (that is a draw
            // to another target): what the pass's draws sample and cannot be redirected for
            // them leaves its atlas now.
            if (!pending.empty()) { // (held-back copies whose pictures the pass samples are made first)
                bool samples = false;
                for (size_t j = i + 1; j < cmds.size() && cmds[j].type != CMD_COPY_TEX && !samples; ++j) {
                    if (cmds[j].type != CMD_DRAW) continue;
                    const DrawRec& d = g_buf.draws[cmds[j].arg];
                    for (int m = 0; m < 8; ++m)
                        if ((d.texMask & (1u << m)) && isPending(d.tex[m].texId)) samples = true;
                }
                if (samples) flush();
            }
            if (!g_atlasOf.empty()) {
                for (size_t j = i + 1; j < cmds.size() && cmds[j].type != CMD_COPY_TEX; ++j) {
                    if (cmds[j].type != CMD_DRAW) continue;
                    DrawRec& d = g_buf.draws[cmds[j].arg];
                    for (int m = 0; m < 8; ++m) {
                        if (!(d.texMask & (1u << m))) continue;
                        auto it = g_atlasOf.find(d.tex[m].texId);
                        if (it == g_atlasOf.end() || AtlasRedirect(d, m, it->second, false)) continue;
                        const uint32_t t = it->first;
                        flush();
                        AtlasExtract(out, t, g_atlasOf.find(t)->second); // (looked up again: flush() may have moved the array)
                        AtlasBarNow(t);
                    }
                }
            }
            inPass = true;
            PassCmd& ps = g_buf.passes[c.arg];
            Kind k{};
            const uint32_t copy = passCopy[c.arg];
            bool ok = classify(ps, copy, k);
            const uint32_t t = ok ? g_buf.copyTex[copy].texId : 0;
            if (ok) ok = !AtlasBarred(t);
            if (ok && !g_atlasOf.count(t)) {
                ok = false;
                for (int q = 0; q < nKinds; ++q)
                    if (sameKind(kinds[q], k)) ok = kinds[q].n >= 2;
            }
            // What the pass's own draws sample: not a picture of the atlas it would draw into
            // (a texture cannot be drawn to and sampled at once); and a pass whose copy is
            // held back must not sample atlas pictures at all (they could be ones whose
            // copies are still held back).
            if (ok) {
                for (size_t j = i + 1; j < cmds.size() && cmds[j].type != CMD_COPY_TEX && ok; ++j) {
                    if (cmds[j].type != CMD_DRAW) continue;
                    const DrawRec& d = g_buf.draws[cmds[j].arg];
                    for (int m = 0; m < 8 && ok; ++m) {
                        if (!(d.texMask & (1u << m))) continue;
                        const uint32_t id = d.tex[m].texId;
                        if (isPending(id)) { ok = false; break; }
                        auto it = g_atlasOf.find(id);
                        if (it == g_atlasOf.end()) continue;
                        if (!k.direct || (g_atlasPages[it->second.page].cellW == k.dw && g_atlasPages[it->second.page].cellH == k.dh)) ok = false;
                    }
                }
            }
            if (!ok || k.direct) flush(); // (a direct pass draws into a picture atlas: the held-back copies go first)
            if (ok && !k.direct) {
                // The scratch atlas for this pass size (gutter: the copy's ratio times the picture gutter).
                const uint16_t g = static_cast<uint16_t>(kAtlasGutter * (k.sw / k.dw));
                int sp = -1;
                for (size_t q = 0; q < g_scratchPages.size(); ++q)
                    if (g_scratchPages[q].cellW == k.sw && g_scratchPages[q].cellH == k.sh && g_scratchPages[q].gutter == g) sp = static_cast<int>(q);
                if (sp < 0) {
                    AtlasPage pg;
                    pg.Init(k.sw, k.sh, g, 512);
                    g_scratchPages.push_back(pg);
                    sp = static_cast<int>(g_scratchPages.size() - 1);
                }
                if (scratch != sp || pending.size() >= g_scratchPages[sp].owner.size()) flush();
                scratch = sp;
                openScratchCell = static_cast<uint16_t>(pending.size());
            }
            if (ok) {
                openLoc = AtlasCellFor(out, t, k.dw, k.dh); // (may put a copy of an evicted picture into `out` first)
                g_atlasPages[openLoc.page].lastUse[openLoc.cell] = g_frame;
                if (k.direct) g_atlasPages[openLoc.page].Target(ps, openLoc.cell);
                else g_scratchPages[scratch].Target(ps, openScratchCell);
                openTex = t;
                openCopy = copy;
                openDirect = k.direct;
                ++fr.atlasPasses;
            } else {
                openTex = 0;
            }
            out.push_back(c);
            break;
        }
        case CMD_DRAW: {
            if (!inPass) flush(); // a draw to the EFB: it may sample what the held-back copies make
            if (!g_atlasOf.empty()) {
                DrawRec* d = &g_buf.draws[c.arg];
                bool remapped = false;
                for (int m = 0; m < 8; ++m) {
                    if (!(d->texMask & (1u << m))) continue;
                    auto it = g_atlasOf.find(d->tex[m].texId);
                    if (it == g_atlasOf.end()) continue;
                    const uint32_t t = it->first;
                    const AtlasLoc loc = it->second;
                    if (AtlasRedirect(*d, m, loc, true)) {
                        remapped = true;
                        g_atlasPages[loc.page].lastUse[loc.cell] = g_frame;
                        continue;
                    }
                    if (inPass) { // (cannot happen: the pass's draws were looked at when it began)
                        VGX_LOG_N(8, "copy atlas: texture %u cannot be redirected for a draw inside a pass (frame %llu)\n", t, static_cast<unsigned long long>(g_frame));
                        continue;
                    }
                    // Take the picture back out: a copy from its cell into its own texture, before this draw.
                    AtlasExtract(out, t, loc);
                    AtlasBarNow(t);
                    VGX_LOG_N(16, "copy atlas: texture %u taken out again at frame %llu (a draw samples it in a way that cannot be redirected: test %d, "
                                  "vbuf %u, %u vertices, layout 0x%X, vs %u fs %u)\n", t, static_cast<unsigned long long>(g_frame), g_atlasWhy, d->vbuf,
                              d->vtxCount, d->layoutMask, d->vsId, d->fsId);
                }
                if (remapped) ++fr.atlasRemaps;
            }
            out.push_back(c);
            break;
        }
        case CMD_COPY_TEX: {
            CopyTexCmd& t = g_buf.copyTex[c.arg];
            if (openTex && c.arg == openCopy) {
                if (openDirect) {
                    g_atlasOf[openTex] = openLoc; // the picture now lives in its cell
                } else {
                    // The copy itself is made later (flush); what stays here is the end of the pass and the clear.
                    pending.push_back(Blit{openTex, c.arg, openLoc, openScratchCell});
                    t.skip = true;
                }
            } else if (!t.skip) {
                flush();
                AtlasRelease(t.texId); // written the ordinary way: a texture of its own again
            }
            openTex = 0;
            inPass = false;
            out.push_back(c);
            break;
        }
        case CMD_TEX_FREE:
            flush();
            AtlasRelease(c.arg);
            g_atlasBar.erase(c.arg);
            out.push_back(c);
            break;
        case CMD_SET_TARGET:
            flush();
            // (the back end makes its copy textures anew for another target size)
            for (std::vector<AtlasPage>* list : {&g_atlasPages, &g_scratchPages})
                for (AtlasPage& pg : *list) {
                    std::fill(pg.owner.begin(), pg.owner.end(), 0u);
                    for (AtlasPage::Gut& g : pg.gut) g.valid = false;
                }
            g_atlasOf.clear();
            out.push_back(c);
            break;
        case CMD_COPY_DISP:
        case CMD_PRESENT:
            flush();
            out.push_back(c);
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    flush();
    // Passes that follow each other directly and draw into different pages come out in page
    // order, so that a page is one scene however its pictures were handed out. Only whole
    // passes move (begin, draws, the copy that ends them); only when their copies leave the
    // EFB the same whatever the order (one rectangle, one clear), and when no draw of them
    // samples a page that one of them draws into.
    {
        struct Grp { uint32_t b, e, page; };
        static std::vector<Grp> run;
        static std::vector<Cmd> tmp;
        size_t i = 0;
        while (i < out.size()) {
            run.clear();
            size_t j = i;
            while (j < out.size() && out[j].type == CMD_PASS_BEGIN) {
                const PassCmd& ps = g_buf.passes[out[j].arg];
                if (!ps.atlasTex || !ps.directTex) break;
                size_t e = j + 1;
                while (e < out.size() && out[e].type == CMD_DRAW) ++e;
                if (e >= out.size() || out[e].type != CMD_COPY_TEX || !g_buf.copyTex[out[e].arg].fromPass) break;
                run.push_back(Grp{static_cast<uint32_t>(j), static_cast<uint32_t>(e + 1), ps.atlasTex});
                j = e + 1;
            }
            bool sorted = true;
            for (size_t q = 1; q < run.size(); ++q)
                if (run[q].page < run[q - 1].page) sorted = false;
            if (!sorted) {
                bool safe = true;
                const CopyTexCmd& c0 = g_buf.copyTex[out[run[0].e - 1].arg];
                for (const Grp& g : run) {
                    const CopyTexCmd& cg = g_buf.copyTex[out[g.e - 1].arg];
                    if (std::memcmp(&cg.src, &c0.src, sizeof cg.src) != 0 || cg.clear.enable != c0.clear.enable || cg.clearElided != c0.clearElided ||
                        (cg.clear.enable && (cg.clear.color != c0.clear.color || cg.clear.alpha != c0.clear.alpha || cg.clear.depth != c0.clear.depth ||
                                             std::memcmp(cg.clear.rgba, c0.clear.rgba, sizeof cg.clear.rgba) != 0 || cg.clear.z != c0.clear.z)))
                        safe = false;
                    for (uint32_t k = g.b + 1; k + 1 < g.e && safe; ++k) {
                        const DrawRec& d = g_buf.draws[out[k].arg];
                        for (int m = 0; m < 8 && safe; ++m) {
                            if (!(d.texMask & (1u << m))) continue;
                            for (const Grp& h : run)
                                if (d.tex[m].texId == h.page || d.tex[m].texId == g_buf.passes[out[h.b].arg].directTex) safe = false;
                        }
                    }
                }
                if (safe) {
                    std::stable_sort(run.begin(), run.end(), [](const Grp& x, const Grp& y) { return x.page < y.page; });
                    tmp.clear();
                    for (const Grp& g : run) tmp.insert(tmp.end(), out.begin() + g.b, out.begin() + g.e);
                    std::copy(tmp.begin(), tmp.end(), out.begin() + static_cast<std::ptrdiff_t>(i));
                    ++fr.atlasSorted;
                }
            }
            i = j > i ? j : i + 1;
        }
    }
    // Draws that follow each other and now differ in nothing but their vertices become one.
    static const bool noMerge = EnvInt("GXV_NOMERGE", 0) != 0;
    size_t n = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        const Cmd c = out[i];
        if (!noMerge && fr.atlasRemaps && c.type == CMD_DRAW && n > 0 && out[n - 1].type == CMD_DRAW) {
            DrawRec& a = g_buf.draws[out[n - 1].arg];
            const DrawRec& b = g_buf.draws[c.arg];
            if (a.vbuf == 0 && b.vbuf == 0 && a.vchunk == b.vchunk && a.vtxOffset == b.vtxOffset && a.prim == b.prim && a.layoutMask == b.layoutMask &&
                a.vsId == b.vsId && a.fsId == b.fsId && a.texMask == b.texMask && a.idxOffset + a.idxCount == b.idxOffset &&
                a.vtxFirst + a.vtxCount == b.vtxFirst && a.vtxCount + b.vtxCount <= 0xFFFF &&
                std::memcmp(a.ublock, b.ublock, sizeof a.ublock) == 0 && std::memcmp(&a.rs, &b.rs, sizeof(RenderState)) == 0 &&
                std::memcmp(a.tex, b.tex, sizeof a.tex) == 0) {
                a.idxCount += b.idxCount;
                a.vtxCount += b.vtxCount;
                g_buf.draws[c.arg].idxCount = 0; // (the record stays in the list, unused)
                g_buf.draws[c.arg].vtxCount = 0;
                ++fr.atlasMerged;
                if (fr.drawCalls) --fr.drawCalls;
                continue;
            }
        }
        out[n++] = c;
    }
    out.resize(n);
    cmds.swap(out);
}
} // namespace

// GXV_PASSCHECK=1 (diagnostic, slow): checks every claim the analysis
// above made about the EFB against a per-pixel model of it. The model follows
// what the back end will do with the finished frame (draws outside passes make
// the planes they write unknown inside their scissor; executed clears set
// their planes) and each claim is tested pixel by pixel: a pass's rectangle
// holds the pass's start values in the planes that matter, a fill's source
// holds the fill colour in the planes the copy reads, an elided clear's
// rectangle already holds the clear's values. The rectangle history is an
// approximation of exactly this model, so any disagreement is a bug in it.
namespace {
constexpr int kChkW = 640, kChkH = 528;
struct PassCheck {
    std::vector<uint8_t> px[3]; // value id per pixel per plane; 0 = unknown
    std::vector<std::array<float, 3>> vals[3];
    uint64_t checks = 0, failures = 0, pixelsChecked = 0;
    EfbRect lastDraw{};
    uint8_t lastDrawPlanes = 0;
    bool lastDrawValid = false;
    void Reset() {
        for (auto& p : px) p.assign(static_cast<size_t>(kChkW) * kChkH, 0);
        lastDrawValid = false;
    }
    uint8_t Id(int plane, float a, float b = 0, float c = 0) {
        auto& v = vals[plane];
        for (size_t i = 0; i < v.size(); ++i)
            if (v[i][0] == a && v[i][1] == b && v[i][2] == c) return static_cast<uint8_t>(i + 1);
        if (v.size() >= 254) return 255; // (never equal to a stored id below 255 unless it is the same overflow bucket)
        v.push_back({a, b, c});
        return static_cast<uint8_t>(v.size());
    }
    static bool Clip(EfbRect r, int& x0, int& y0, int& x1, int& y1) {
        x0 = std::max<int>(r.x, 0); y0 = std::max<int>(r.y, 0);
        x1 = std::min<int>(r.x + r.w, kChkW); y1 = std::min<int>(r.y + r.h, kChkH);
        return x1 > x0 && y1 > y0;
    }
    void Set(int plane, const EfbRect& r, uint8_t id) {
        int x0, y0, x1, y1;
        if (!Clip(r, x0, y0, x1, y1)) return;
        for (int y = y0; y < y1; ++y) std::memset(&px[plane][static_cast<size_t>(y) * kChkW + x0], id, static_cast<size_t>(x1 - x0));
    }
    bool Holds(int plane, const EfbRect& r, uint8_t id) {
        int x0, y0, x1, y1;
        if (!Clip(r, x0, y0, x1, y1)) return true;
        for (int y = y0; y < y1; ++y) {
            const uint8_t* row = &px[plane][static_cast<size_t>(y) * kChkW];
            for (int x = x0; x < x1; ++x)
                if (row[x] != id) return false;
        }
        pixelsChecked += static_cast<uint64_t>(x1 - x0) * (y1 - y0);
        return true;
    }
    bool HoldsValues(uint8_t planes, const EfbRect& r, const float rgba[4], float z) {
        bool ok = true;
        if (planes & CH_C) ok = Holds(0, r, Id(0, rgba[0], rgba[1], rgba[2])) && ok;
        if (planes & CH_A) ok = Holds(1, r, Id(1, rgba[3])) && ok;
        if (planes & CH_Z) ok = Holds(2, r, Id(2, z)) && ok;
        return ok;
    }
    void Claim(bool ok, const char* what, const EfbRect& r) {
        ++checks;
        if (ok) return;
        ++failures;
        VGX_LOG_N(40, "PASSCHECK FAILED frame %llu: %s, rectangle (%d,%d)-(%d,%d)\n", static_cast<unsigned long long>(g_frame), what, r.x, r.y, r.x + r.w,
                  r.y + r.h);
    }
    void ApplyClear(const EfbRect& r, const ClearSpec& c, bool elided) {
        if (elided) {
            const uint8_t m = static_cast<uint8_t>((c.color ? CH_C : 0) | (c.alpha ? CH_A : 0) | (c.depth ? CH_Z : 0));
            Claim(HoldsValues(m, r, c.rgba, c.z), "elided clear is not a no-op", r);
            return;
        }
        if (!c.enable) return;
        if (c.color) Set(0, r, Id(0, c.rgba[0], c.rgba[1], c.rgba[2]));
        if (c.alpha) Set(1, r, Id(1, c.rgba[3]));
        if (c.depth) Set(2, r, Id(2, c.z));
        lastDrawValid = false;
    }
} g_chk;
} // namespace

static void VerifyPasses() {
    static const bool on = diag::Int("GXV_PASSCHECK", 0) != 0;
    if (!on) return;
    if (g_chk.px[0].empty()) g_chk.Reset();
    bool inPass = false;
    for (const Cmd& c : g_buf.cmds) {
        switch (c.type) {
        case CMD_PASS_BEGIN: {
            const PassCmd& ps = g_buf.passes[c.arg];
            g_chk.Claim(g_chk.HoldsValues(ps.known, ps.rect, ps.rgba, ps.z), "pass does not start from the values given", ps.rect);
            inPass = true;
            break;
        }
        case CMD_DRAW: {
            if (inPass) break;
            const RenderState& rs = g_buf.draws[c.arg].rs;
            const EfbRect sc{rs.scX, rs.scY, rs.scW, rs.scH};
            const uint8_t w = DrawWrites(rs);
            if (g_chk.lastDrawValid && std::memcmp(&sc, &g_chk.lastDraw, sizeof sc) == 0 && (w & ~g_chk.lastDrawPlanes) == 0) break;
            for (int p = 0; p < 3; ++p)
                if (w & (1u << p)) g_chk.Set(p, sc, 0);
            if (!g_chk.lastDrawValid || std::memcmp(&sc, &g_chk.lastDraw, sizeof sc) != 0) g_chk.lastDrawPlanes = 0;
            g_chk.lastDraw = sc;
            g_chk.lastDrawPlanes |= w;
            g_chk.lastDrawValid = true;
            break;
        }
        case CMD_COPY_TEX: {
            const CopyTexCmd& t = g_buf.copyTex[c.arg];
            inPass = false;
            if (t.fill) g_chk.Claim(g_chk.HoldsValues(CopyReads(t), t.src, t.fillRgba, 1.0f), "fill source is not that constant", t.src);
            g_chk.ApplyClear(t.src, t.clear, t.clearElided);
            break;
        }
        case CMD_COPY_DISP:
            g_chk.ApplyClear(g_buf.copyDisp[c.arg].src, g_buf.copyDisp[c.arg].clear, g_buf.copyDisp[c.arg].clearElided);
            break;
        case CMD_SET_TARGET:
            g_chk.Reset();
            break;
        default: break;
        }
    }
}
void PassCheckReport(FILE* f) {
    if (g_passRepeat.frames)
        std::fprintf(f, "pass repeats (GXV_PASSREPEAT): %llu offscreen passes in %llu frames; %llu drew exactly what the pass into the same texture drew "
                        "the frame before, %llu what another pass of the same frame drew\n",
                     static_cast<unsigned long long>(g_passRepeat.passes), static_cast<unsigned long long>(g_passRepeat.frames),
                     static_cast<unsigned long long>(g_passRepeat.sameAsLastFrame), static_cast<unsigned long long>(g_passRepeat.sameAsOther));
    if (g_chk.px[0].empty()) return;
    std::fprintf(f, "pass check (GXV_PASSCHECK): %llu claims tested against the per-pixel model (%llu pixel-plane comparisons), %llu FAILED\n",
                 static_cast<unsigned long long>(g_chk.checks), static_cast<unsigned long long>(g_chk.pixelsChecked),
                 static_cast<unsigned long long>(g_chk.failures));
}

// Statistics: how often the finished frame makes a back end switch render
// targets (the order of operations is the GL back end's: a copy draws into its
// destination, then its clear goes to the EFB).
static void CountScenes() {
    if (!stats::Detailed()) return;
    stats::Frame& fr = stats::Cur();
    bool inPass = false;
    uint32_t passDirectTex = 0;
    uint32_t target = 0; // 0: none yet, 1: the EFB, 2: some other target, else: an atlas (texture id + 16)
    auto efb = [&] { if (target != 1) { ++fr.efbScenes; target = 1; } };
    auto other = [&](uint32_t id) { if (id == 2 || target != id) ++fr.offScenes; target = id; }; // (passes into one atlas in a row: one scene)
    for (const Cmd& c : g_buf.cmds) {
        switch (c.type) {
        case CMD_PASS_BEGIN:
            other(g_buf.passes[c.arg].atlasTex ? g_buf.passes[c.arg].atlasTex + 16 : 2);
            inPass = true;
            passDirectTex = g_buf.passes[c.arg].directTex;
            break;
        case CMD_DRAW: if (!inPass) efb(); break;
        case CMD_COPY_TEX: {
            const CopyTexCmd& t = g_buf.copyTex[c.arg];
            if (!t.skip) {
                // a copy quad or a fill into the destination (a direct pass has drawn there already)
                const bool direct = t.fromPass && inPass && passDirectTex == t.texId;
                if (!direct) other(t.dstTexW ? t.texId + 16 : 2);
            }
            if (!t.srcTex) inPass = false;
            if (t.clear.enable) efb();
            break;
        }
        case CMD_COPY_DISP:
            if (g_buf.copyDisp[c.arg].clear.enable) efb();
            break;
        default: break;
        }
    }
}

// GXV_PASSES=<frame> (diagnostic): the frame as a sequence of render
// passes: runs of draws (count, triangles, union of their scissor rectangles,
// copy textures of this frame they sample) separated by EFB copies. For
// planning a back end for a tile-based GPU, where every copy that interrupts
// drawing to the EFB costs a store and a reload of the whole render target.
static void TracePasses() {
    std::vector<uint32_t> copyIds;
    for (const CopyTexCmd& c : g_buf.copyTex) copyIds.push_back(c.texId);
    auto isCopy = [&](uint32_t id) { return std::find(copyIds.begin(), copyIds.end(), id) != copyIds.end(); };
    int n = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint8_t wr = 0, rd = 0;
    uint32_t tris = 0;
    std::vector<uint32_t> sampled;
    auto flush = [&] {
        if (!n) return;
        std::fprintf(stderr, "[passes]   %3d draws %6u tris  bound (%d,%d)-(%d,%d)  writes %s%s%s reads %s%s%s", n, tris, x0, y0, x1, y1,
                     wr & CH_C ? "c" : "-", wr & CH_A ? "a" : "-", wr & CH_Z ? "z" : "-", rd & CH_C ? "c" : "-", rd & CH_A ? "a" : "-", rd & CH_Z ? "z" : "-");
        if (!sampled.empty()) {
            std::fprintf(stderr, "  samples copies:");
            for (uint32_t id : sampled) std::fprintf(stderr, " %u", id);
        }
        std::fprintf(stderr, "\n");
        n = 0; tris = 0; wr = rd = 0; sampled.clear();
    };
    std::fprintf(stderr, "[passes] frame %llu: %u commands, %u draws, %u texture copies\n", static_cast<unsigned long long>(g_frame), static_cast<unsigned>(g_buf.cmds.size()),
                 static_cast<unsigned>(g_buf.draws.size()), static_cast<unsigned>(g_buf.copyTex.size()));
    for (const Cmd& c : g_buf.cmds) {
        if (c.type == CMD_DRAW) {
            const DrawRec& d = g_buf.draws[c.arg];
            const int ax = d.exX, ay = d.exY, bx = d.exX + d.exW, by = d.exY + d.exH;
            if (d.cover || d.exW != d.rs.scW || d.exH != d.rs.scH || d.idxCount <= 6)
                std::fprintf(stderr, "[passes]     draw %u: %u tris, bound (%d,%d)-(%d,%d)%s, blend %d (%d,%d) depth test %d func %d write %d, colour/alpha write %d/%d, cull %d, vs %u fs %u tex %u/%u/%u mask 0x%X\n", c.arg,
                             d.idxCount / 3, ax, ay, bx, by, d.cover ? " COVER" : "", d.rs.blend, d.rs.blendSrc, d.rs.blendDst, d.rs.depthTest, d.rs.depthFunc, d.rs.depthWrite,
                             d.rs.colorWrite, d.rs.alphaWrite, d.rs.cull, d.vsId, d.fsId, d.tex[0].texId, d.tex[1].texId, d.tex[2].texId, d.texMask);
            if (!n) { x0 = ax; y0 = ay; x1 = bx; y1 = by; }
            else { x0 = std::min(x0, ax); y0 = std::min(y0, ay); x1 = std::max(x1, bx); y1 = std::max(y1, by); }
            ++n;
            wr |= DrawWrites(d.rs);
            rd |= DrawReads(d.rs);
            tris += d.idxCount / 3;
            for (int m = 0; m < 8; ++m)
                if ((d.texMask & (1u << m)) && isCopy(d.tex[m].texId) && std::find(sampled.begin(), sampled.end(), d.tex[m].texId) == sampled.end())
                    sampled.push_back(d.tex[m].texId);
        } else if (c.type == CMD_COPY_TEX) {
            flush();
            const CopyTexCmd& t = g_buf.copyTex[c.arg];
            char cl[96] = "";
            if (t.clear.enable)
                std::snprintf(cl, sizeof cl, " +clear %s%s%s (%.3f,%.3f,%.3f,%.3f) z %.4f", t.clear.color ? "c" : "-", t.clear.alpha ? "a" : "-", t.clear.depth ? "z" : "-",
                              t.clear.rgba[0], t.clear.rgba[1], t.clear.rgba[2], t.clear.rgba[3], t.clear.z);
            std::fprintf(stderr, "[passes] COPY -> tex %u  src (%d,%d)-(%d,%d)  dst %ux%u%s%s%s%s%s\n", t.texId, t.src.x, t.src.y, t.src.x + t.src.w, t.src.y + t.src.h,
                         t.dstW, t.dstH, t.depth ? " depth" : "", cl, t.skip ? (t.fill ? " (texture already holds it: clear only)" : " (dead: clear only)") : "",
                         t.fromPass ? "  [ends isolated pass]" : "", t.fill ? "  [fill: constant source]" : "");
        } else if (c.type == CMD_PASS_BEGIN) {
            flush();
            const PassCmd& ps = g_buf.passes[c.arg];
            std::fprintf(stderr, "[passes] PASS BEGIN rect (%d,%d)-(%d,%d) cleared to (%.3f,%.3f,%.3f,%.3f) z %.4f%s\n", ps.rect.x, ps.rect.y, ps.rect.x + ps.rect.w,
                         ps.rect.y + ps.rect.h, ps.rgba[0], ps.rgba[1], ps.rgba[2], ps.rgba[3], ps.z, ps.directTex ? "  [straight into the destination texture]" : "");
        } else if (c.type == CMD_COPY_DISP) {
            flush();
            const CopyDispCmd& t = g_buf.copyDisp[c.arg];
            std::fprintf(stderr, "[passes] DISPLAY COPY src (%d,%d)-(%d,%d)%s\n", t.src.x, t.src.y, t.src.x + t.src.w, t.src.y + t.src.h, t.clear.enable ? " +clear" : "");
        }
    }
    flush();
}

void SetShaderListFile(const char* path) {
    ListFlush();
    g_shaderListPath = path ? path : "";
}
void SetShaderWarmupDeferred(bool deferred) { g_warmDeferred = deferred; }
#if GXV_RECIPE_SEED_BATCH
void SetShaderRecipeSeedFile(const char* path) { g_recipeSeedPath = path ? path : ""; }
#endif

static void (*g_frameSink)(CmdBuf&) = nullptr;
void SetFrameSink(void (*sink)(CmdBuf& buf)) { g_frameSink = sink; }

void FrontPresent(bool direct, void (*onDecoded)()) {
    if (!g_shaderListDone) EnsureShaderList();
    TexCacheEndFrame();
    // (front-end split: the finishing thread's records, indices and uniforms join the frame here; a no-op when the
    // host did it at the display copy already)
    if (split::g_on) split::FinishFrame(g_buf);
    g_buf.cmds.push_back(Cmd{CMD_PRESENT, 0});
    g_buf.frame = g_frame;
    g_buf.crowdActive = StaticCrowdEnabled();
    if (g_buf.crowdActive) {
        g_buf.crowdObservations = g_crowdObserved ? 1u : 0u;
        g_buf.crowdTags = g_crowdTagCount;
        g_buf.crowdInvalid = g_crowdInvalid;
    }
    MarkDeadCopies();
    IsolatePasses();
    AtlasPasses();
    VerifyPasses();
    CountScenes();
    {
        // GXV_PASSES=<frame>[,<frame>...]
        static const char* const list = diag::Env("GXV_PASSES");
        for (const char* q = list; q && *q;) {
            char* end = nullptr;
            const unsigned long long f = std::strtoull(q, &end, 10);
            if (end == q) break;
            if (f == g_frame) TracePasses();
            q = *end == ',' ? end + 1 : end;
        }
    }
    {
        stats::Frame& fr = stats::Cur();
        fr.vtxBytes += static_cast<uint32_t>(g_buf.vtx.size());
        fr.idxBytes += static_cast<uint32_t>(g_buf.idx.size() * 2);
        fr.uniBytes += static_cast<uint32_t>(g_buf.uni.size() * 4);
        static const long hashMode = diag::Int("GXV_HASH", 0);
        if (hashMode) fr.hash = HashBuf(g_buf, fr.hash, hashMode == 2);
    }
    if (LightNormCensusOn() && (g_frame == 1 || g_frame % 600 == 0))
        std::fprintf(stderr, "[vgx] light directions up to frame %llu: %llu blocks built; %llu non-zero directions written as zero (%llu of them non-finite, %llu not read by the shader at all); %llu kept (%llu non-finite)\n",
                     (unsigned long long)g_frame, (unsigned long long)g_lnc.blocks, (unsigned long long)g_lnc.zeroed, (unsigned long long)g_lnc.zeroedNonFinite,
                     (unsigned long long)g_lnc.zeroedUnread, (unsigned long long)g_lnc.kept, (unsigned long long)g_lnc.keptNonFinite);
    stats::FrameEnd(g_frame);
    {
        static const long n = diag::Int("GXV_TRACE_DRAWS", 0);
        static bool done = false;
        if (n > 0 && !done && g_frameDraws >= static_cast<uint32_t>(n)) {
            done = true;
            g_traceFrame = g_frame + 1;
            std::fprintf(stderr, "[trace] tracing frame %llu (previous frame had %u draws)\n", static_cast<unsigned long long>(g_traceFrame), g_frameDraws);
        }
        g_frameDraws = 0;
    }
    g.texDirty = true; // re-resolve bindings at least once per frame
    g.texSlotDirty = 0xFF;
    ++g_frame;
    g_traceNow = -1; // (TraceFrame: asked again for the new frame)
    // Everything of this frame has been read from guest memory: the game
    // thread may go on while the back end draws.
    if (onDecoded) onDecoded();
    if (g_frameSink) g_frameSink(g_buf);
    else if (direct) backend::Execute(g_buf);
    else SubmitToBackend(g_buf);
    g_buf.clear();
    if (StaticCrowdEnabled()) ResetCrowdTags();
    g_blocksStale = true;
    for (uint32_t& o : g_blockOff) o = kNoBlock;
    g_lastDraw = -1;
    split::FrameBegin(); // (whether the next frame runs split)
    // Release the producer and submit GPU work before checkpointing newly learned recipes.
    ListFlush();
}

void QueueSetTarget() {
    SetTargetCmd t;
    t.efbW = g.efbW;
    t.efbH = g.efbH;
    g_buf.targets.push_back(t);
    g_buf.cmds.push_back(Cmd{CMD_SET_TARGET, static_cast<uint32_t>(g_buf.targets.size() - 1)});
    g_lastDraw = -1;
}

} // namespace vgx
