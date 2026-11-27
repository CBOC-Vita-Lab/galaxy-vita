// Front end: turns the register state into self-contained draw records.
#include "vgx_workload.h"
#include "../../include/port/fpbits.h" // (neonbox40)
#include "../../include/port/getenv25.h" // (getenv25: GXV_OVERSCAN / GXV_PREVCAP read once, not per marked capture)
#include "../../include/port/shadow_stencil.h" // (dm65: the stencil counter's width)
#include "vgx_tinyrun.h" // (dread25: GXV_TINYRUN)
#include "vgx_facecull.h" // (farlod32: GXV_FACECULL)
#include "vgx_prepass.h" // (dread25: GXV_ALPHATEST_PREPASS)
#include "vgx_shed.h"
#include "vgx_hrw_capfix.h" // (GXV_HRW_CAPFIX)
#include "vgx_indcap_vis.h" // (GXV_INDCAP_VIS)
#include "vgx_indcap_peek.h" // (GXV_INDCAP_PEEK: the front end only writes the marker; the render thread decides)
#include "vgx_state.h"
#include "vgx_arena.h"
#include "vgx_fesplit.h"
#include "vgx_fepar.h"
#include "vgx_fixes.h"
#include "vgx_recipe_seed.h"
#include "vgx_variant_set.h"
#include "vgx_keymemo.h"
#include "vgx_indflat.h"
#include "vgx_direct.h"
#include "vgx_directdisplay.h" // (sg12: dd::EffectiveMode, the HRW guard)
#include "vgx_bloombracket.h" // (bloom37: GXV_BLOOM_BRACKET)
#include "vgx_j3n2.h"
#include "vgx_twocore.h"
#include "vgx_fillshrink.h" // (gpugen26: GXV_SHADOWFILL_SHRINK)
#include "vgx_silhfuse.h" // (silhfuse26: GXV_SILH_FUSE)
#include "vgx_stage_enum.h" // (precomp26: GXV_STAGE_ENUM)
#include "vgx_paljoin.h" // (paljoin31: GXV_PALJOIN_CENSUS)
#if !defined(VGX_CLONE_B) // (the second worker never makes a shader text: vgx_twocore_b_draw.cpp)
#include "vgx_ubershader.h" // (asyncsh31: GXV_UBERSHADER census)
#endif
#include <type_traits>
#include "vgx_keys27.h" // (keys27: GXV_KEYS27, the general-form links of the value-driven variants)
#include "../../include/port/stage_knob.h"
#include "../../include/port/glow26.h" // (glow26: GXV_GLOW26)

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <pthread.h>
#include <string>
#include <cstring>
#include <cstdio>
extern "C" int port_card_write_async(const char* path, int mode, const void* data, unsigned bytes) __attribute__((weak));  // (hitch30: platform/src/os/os_alarm.cpp, the card-write queue)

extern "C" uint32_t port_scene_key(void) __attribute__((weak));

namespace vgx {

namespace {

CmdBuf g_buf;
uint64_t g_frame = 1;

// (dma31) The front end's clock. Each log line below checks "is it time for my line yet" once a picture; each check used to read the
// clock itself (a system call on the console, ~0.87 us). They now share one read per picture: FeClock::Picture() reads the clock the
// first time it is asked in a picture (g_frame) and gives that time to every later question of the same picture (a line is written at
// most one picture later than before; nothing but the log lines' timing uses it). Scopes that time work (GXV_DYN_CENSUS) read the
// clock themselves, through FeClock::Read(). This struct is the only place of the file that names the clock (tests/test_player_audit.py).
struct FeClock {
    using Time = std::chrono::steady_clock::time_point;
    static Time Read() { return std::chrono::steady_clock::now(); }
    static Time Picture() {
        // (atomics: the time is two words on the console; a log check on another thread must not see half of one)
        static std::atomic<int64_t> ticks{0};
        static std::atomic<uint64_t> frame{0};
        if (frame.load(std::memory_order_acquire) != g_frame) {
            const Time now = Read();
            ticks.store(static_cast<int64_t>(now.time_since_epoch().count()), std::memory_order_relaxed);
            frame.store(g_frame, std::memory_order_release);
            return now;
        }
        return Time(Time::duration(ticks.load(std::memory_order_relaxed)));
    }
};

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
    // Preserve legacy identities; the new blend bit occupies the formerly unused bit 24.
    return (static_cast<uint64_t>((vsId << 16) | fsId) << 32) | ((layoutMask & 0xFFFu) << 12) | (blendKey & 0xFFFu) | ((blendKey & 0x1000u) << 12);
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
// hitch30: the list's appends go to the card-write queue (os_alarm.cpp, port/card_queue.h). The fwrite + fflush of a picture's new records
// (12-15 ms on the console, and 200+ ms when the card is busy) ran on the thread that ends the picture. GXV_CARD_ASYNC=0 writes on that thread as before.
bool CardAsyncOn() {
    static const bool on = EnvInt("GXV_CARD_ASYNC", 1) != 0 && port_card_write_async != nullptr;
    return on;
}
bool ListFlush() {
    if (!g_shaderListPendingSize) return true;
    if (g_shaderList && CardAsyncOn()) {
        // A full queue keeps the records here (nothing is lost, the next picture asks again; only a full 16 KiB buffer, in ListQueue, ends learning).
        if (!port_card_write_async(g_shaderListPath.c_str(), 1, g_shaderListPending.data(), static_cast<unsigned>(g_shaderListPendingSize))) return false;
        g_shaderListPendingSize = 0;
        return true;
    }
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
    if (g_shaderListPendingSize > g_shaderListPending.size() - sizeof type - n && !ListFlush()) {
        if (g_shaderList) { ListDisable(); VGX_ERR("shader recipe learning stopped: the card-write queue was full and the pending list buffer too\n"); }  // (hitch30: ListFlush kept the records; a gap would break the list's order)
        return false;
    }
    std::memcpy(g_shaderListPending.data() + g_shaderListPendingSize, &type, sizeof type);
    g_shaderListPendingSize += sizeof type;
    std::memcpy(g_shaderListPending.data() + g_shaderListPendingSize, p, n);
    g_shaderListPendingSize += n;
    return true;
}
bool g_enumDry = false; // (precomp26, see StageEnumDrawMarker: true only while its dry draw is resolved; a shader text made then is flagged enumOnly)
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
#if defined(VGX_CLONE_B)
    {
        // (GXV_FE_TWOCORE, the second worker: its registry is a mirror of the player's, by the player's ids, filled from it; a key the
        // player has not made ends the segment: the player makes it, in stream order)
        two::LockB(); // (twocore24: or B gives its picture up, two::kAbandonLock)
        const int id = vgx2_reg_find(fragment ? 1 : 0, &key, static_cast<uint32_t>(sizeof(K)), h);
        vgx2_unlock_b();
        if (id < 0) two::Abandon(2);
        reg.map.emplace(h, typename Registry<K>::Entry{key, static_cast<uint16_t>(id)});
        if (reg.keys.size() <= static_cast<size_t>(id)) reg.keys.resize(static_cast<size_t>(id) + 1);
        reg.keys[static_cast<size_t>(id)] = key;
        fr.h = h; fr.id = static_cast<uint16_t>(id); fr.valid = true;
        (void)gen;
        return static_cast<uint16_t>(id);
    }
#endif
    two::AGuard lockedForTheSecondWorker; // (GXV_FE_TWOCORE: it reads the registry)
    auto src = std::make_unique<ShaderSource>();
    src->fragment = fragment;
    src->id = static_cast<uint16_t>(reg.sources.size());
    src->keyHash = h;
    src->enumOnly = g_enumDry;
    gen(key, *src);
#if !defined(VGX_CLONE_B)
    if constexpr (std::is_same_v<K, FsKey>) { // (asyncsh31, GXV_UBERSHADER=1: the census's "would the ubershader take it")
        if (UberMode()) {
            const char* why = "";
            src->uber = UberFsCovers(key, &why) ? 1 : 2;
            src->uberWhy = why;
            src->uberIface = src->uber == 1 ? UberIfaceOf(key).Word() : 0u;
        }
    }
#endif
    g_buf.shaders.push_back(src.get());
    ReplayCmds().push_back(Cmd{CMD_SHADER, static_cast<uint32_t>(g_buf.shaders.size() - 1)});
    reg.map.emplace(h, typename Registry<K>::Entry{key, src->id});
    reg.keys.push_back(key);
    reg.sources.push_back(std::move(src));
    if constexpr (std::is_same<K, FsKey>::value) keys27::NoteKey(static_cast<uint32_t>(reg.keys.size() - 1), key); // (keys27: the key's shape and value bits, for the forensics)
    else keys27::Hook(0, static_cast<uint32_t>(reg.keys.size() - 1), &key, static_cast<uint32_t>(sizeof key));
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

// keys27 (GXV_KEYS27_EXPAND): the siblings a draw can reach from each recorded program, as programs the warm-up links. The rules are the front end's own (RejectZeroFs / AlphaDeadFs / AtPassFs /
// FoldRegs / BuildFsKey's wrap), applied to the key alone; a sibling no draw would make is only a wasted compile, never a wrong picture. "keys27: expand ..." line.
bool FoldKeysOk(const FsKey& fs, const VsKey& vs);
void K27ExpandPairs(const std::vector<uint32_t>& pairs) {
    uint32_t made = 0, wrap = 0, folded = 0;
    std::vector<uint32_t> out;
    for (uint32_t pk : pairs) {
        const uint16_t vs = static_cast<uint16_t>(pk >> 16), fs = static_cast<uint16_t>(pk & 0xFFFFu);
        const FsKey base = g_fsReg.keys[fs]; // (a copy: Lookup grows the registry)
        const bool foldOk = FoldKeysOk(base, g_vsReg.keys[vs]);
        FsKey sib[8];
        int n = keys27::Siblings(base, foldOk, sib, 8);
        const int plain = n;
        if (!(base.inWrap & 1) && FixTevIn() && FsTevInNeedsMark(base, 0xFu, true)) { // (a register outside 0..255 reaches this key)
            for (int i = -1; i < plain && n < 8; ++i) {
                FsKey w = i < 0 ? base : sib[i];
                if (w.silhFuse() || w.preAlpha()) continue;
                w.inWrap = 1;
                if (TevInCanonMode() == 2) w.setPreWrap(FsTevInPreWrapMask(w));
                if (TevInCanonMode() == 2 && TevInPrewrapMode()) w.setDualWrap(FsTevInDualMask(w));
                if (n < 8) sib[n++] = w;
            }
        }
        for (int i = 0; i < n; ++i) {
            CanonListFsKey(sib[i]);
            const uint16_t id = Lookup(g_fsReg, sib[i], true, GenerateFragmentShader);
            const uint32_t q = (static_cast<uint32_t>(vs) << 16) | id;
            if (g_vsReg.keys[vs].numTexGens != g_fsReg.keys[id].numTexGens) continue;
            if (g_progSeen.insert(q).second) {
                ReplayCmds().push_back(Cmd{CMD_PROGRAM, q});
                ++made;
                if (sib[i].regFromVtx) ++folded;
                if (sib[i].inWrap & 1) ++wrap;
            }
        }
    }
    VGX_LOG("keys27: expand: %u recorded programs, %u sibling programs added (%u folded, %u wrap), %u fragment shaders now\n", static_cast<unsigned>(pairs.size()), made, folded, wrap,
            static_cast<unsigned>(g_fsReg.sources.size()));
    (void)out;
}

// seedgate31 (A0): the profile stamp, once per run, from the first shader-list pass (every start path goes through it, the environment is final by then).
// "profile: word=<16 hex> gen=<n> switches=<n>" and "profile-sw: NAME=value ..." lines (at most ~180 characters each): the word is the hash of every switch that changes a
// shader key or text (vgx_shadergen.h VGX_TEXT_SWITCHES) and the generator version. tools/pack24/refill_pack.py refuses a harvest whose word is not the release tree's.
// Log only: nothing here changes a key, a draw, a queue or a file.
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
void LogProfileStamp() {
    static bool done = false;
    if (done) return;
    done = true;
    EmitProfileLines([](const char* l) { if (port_log_summary) port_log_summary("%s", l); else std::fprintf(stderr, "[vgx] %s\n", l); });
}
void EnsureShaderList() {
    LogProfileStamp();
    g_shaderListDone = true;
#if defined(VGX_CLONE_B)
    return; // (GXV_FE_TWOCORE: the warm-up list is the player's; vgx2_variant_known)
#endif
    if (g_shaderListPath.empty()) {
        if (const char* e = std::getenv("GXV_SHADERLIST")) g_shaderListPath = e;
    }
    // keys27 (GXV_KEYS27_COMPILELIST=<file>, the farm's "compile this key list" mode, console and emulator): the file (the shader list format: keys and programs; the farm's merged list, or a
    // learned one) is replayed INSTEAD of the shader list, read-only (never rewritten, never appended to), and with GXV_KEYS27_EXPAND (default 1 in this mode) every program also gets its
    // siblings (rejectZero / atPass / alphaDead, the folded forms, the wrap form of a key a register outside 0..255 can reach): the back end links (so compiles) them all behind the warm-up.
    const char* k27List = std::getenv("GXV_KEYS27_COMPILELIST");
    const bool k27Ro = k27List && *k27List;
    if (k27Ro) g_shaderListPath = k27List;
    const char* k27ExpE = std::getenv("GXV_KEYS27_EXPAND");
    const bool k27Expand = (k27ExpE && *k27ExpE) ? *k27ExpE != '0' : k27Ro;
    if (g_shaderListPath.empty()) return;
#if GXV_RECIPE_SEED_BATCH
    if (TryRecipeMerge()) return;
#endif
    bool rewrite = true; // the file is missing, from another generator, damaged or has duplicates
    uint32_t nVs = 0, nFs = 0, nProg = 0, nVar = 0;
    if (FILE* f = std::fopen(g_shaderListPath.c_str(), "rb")) {
        std::vector<char> ioBuf(65536);
        std::setvbuf(f, ioBuf.data(), _IOFBF, ioBuf.size());
        ListHeader hd;
        if (std::fread(&hd, sizeof hd, 1, f) == 1 && std::memcmp(hd.magic, kListMagic, 8) == 0 && hd.vsKeySize == sizeof(VsKey) &&
            hd.fsKeySize == sizeof(FsKey) && hd.genVersion == kShaderGenVersion) {
            rewrite = false;
            g_shaderListReplay = true;
            std::vector<uint16_t> vsIds, fsIds; // id in this run of the n-th key in the file
            std::vector<uint32_t> k27Pairs;     // (keys27 GXV_KEYS27_EXPAND: the program pairs of the file)
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
                    // (canon24b, GXV_TEVIN_CANON=2: a marked key of a list made under =0 / =1 is warmed as the key BuildFsKey now makes for that
                    // material, with its preWrap mask; the list's own form would never be drawn. A =2 list read under =0 / =1 loses the mask
                    // for the same reason. The file is then written anew with the keys as drawn.)
                    if (CanonListFsKey(k)) rewrite = true;
                    const uint16_t id = Lookup(g_fsReg, k, true, GenerateFragmentShader);
                    if (id != fsIds.size()) rewrite = true;
                    fsIds.push_back(id);
                } else if (type == 3) {
                    uint32_t v[2];
                    if (std::fread(v, sizeof v, 1, f) != 1 || v[0] >= vsIds.size() || v[1] >= fsIds.size()) { rewrite = true; break; }
                    // (a pair whose shaders disagree on the texture coordinates is never made by a draw, BuildVsKey takes the fragment
                    // key's count: a damaged list. It ends the read and the list is written anew; from Producdevity's fork, 9624529)
                    if (g_vsReg.keys[vsIds[v[0]]].numTexGens != g_fsReg.keys[fsIds[v[1]]].numTexGens) { rewrite = true; break; }
                    const uint32_t pk = (static_cast<uint32_t>(vsIds[v[0]]) << 16) | fsIds[v[1]];
                    if (g_progSeen.insert(pk).second) ReplayCmds().push_back(Cmd{CMD_PROGRAM, pk});
                    else rewrite = true;
                    if (k27Expand) k27Pairs.push_back(pk);
                } else if (type == 4) {
                    uint32_t v[4];
                    if (std::fread(v, sizeof v, 1, f) != 1 || v[0] >= vsIds.size() || v[1] >= fsIds.size() || v[2] > 0xFFFu || v[3] > 0x1FFFu) { rewrite = true; break; }
                    if (g_vsReg.keys[vsIds[v[0]]].numTexGens != g_fsReg.keys[fsIds[v[1]]].numTexGens) { rewrite = true; break; }
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
            if (k27Expand) K27ExpandPairs(k27Pairs); // (still in replay mode: nothing of it is appended to a list)
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
    } else if (k27Ro) {
        VGX_LOG("keys27: GXV_KEYS27_COMPILELIST %s does not exist\n", g_shaderListPath.c_str());
        return;
    }
    if (rewrite && !k27Ro) {
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
                                         (static_cast<uint32_t>(vk) & 0xFFFu) | (static_cast<uint32_t>(vk >> 12) & 0x1000u)};
                written = std::fwrite(rec, sizeof rec, 1, f) == 1;
            }
            const bool closed = std::fclose(f) == 0;
            if (!written || !closed) return;
        } else return;
    }
    if (!k27Ro) g_shaderList = std::fopen(g_shaderListPath.c_str(), "ab");
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
    uint8_t palCls = 0; // what the vertex key was built with: 0 the full palette, 1 the short one, 2 the small one (PalClass)
    uint32_t vcdMtxMask = 0;
    uint32_t vcdGen = 0xFFFFFFFFu; // State::vcdGen when ResolveState last ran
    uint32_t stateSerial = 0; // bumped whenever rs, tex or texMask are built again (a record made at this serial has the same ones)
    uint16_t texW[8] = {}, texH[8] = {};
    // What each texmap resolves to, kept while its binding is unchanged (State::texSlotDirty):
    // a draw that changes one texmap, or only the TEV setup, does not look the others up again.
    uint8_t slotResolved = 0;
    TexBind slotBind[8];
    uint16_t slotW[8] = {}, slotH[8] = {};
    // pix23 (PixUseFs): what the draw's values prove. pixZero: 1 its alpha test fails for every fragment, 2 its blend leaves every value
    // it writes as it was (GXV_PIX_ZERO leaves it out); pixAt: its alpha test passes for every fragment (GXV_PIX_ATPASS draws it with the
    // atPass variant); pixTest: its fragment shader has an alpha test (census). pixBase: the fragment shader id they were decided for;
    // pixRegs: TEV registers 1 and 2 as they were then (a folded draw carries them in its vertices: PixFoldRecheck).
    uint8_t pixZero = 0;
    uint8_t pixVtx = 0; // bit ch: the blend could be proven a no-op from this draw's own vertex colours (channel ch read, from the vertices)
    bool pixAt = false, pixTest = false;
    uint16_t pixBase = 0;
    int16_t pixRegs[8] = {};
    // gpu960 (PixDecide / PixUseFs): pixBlendWant: what the draw's values prove about its blend (1 it overwrites: exact; 2 within GXV_PIX_BLENDOFF_LEVELS);
    // pixBlendOff: it is being drawn unblended (cur.rs.blend was cleared; pixSavedBlend is what it had, put back by PixBlendRestore, forgotten when
    // cur.rs is built again); pixCls: the census bits (PIXC_*); pixAlphaLo / pixAlphaHi / pixFs: the alpha bounds and the fragment shader of the census row.
    uint8_t pixBlendWant = 0, pixBlendOff = 0, pixCls = 0, pixBis = 0, diagFetch = 0; // (pixBis: GXV_GPU_BISECT gave the draw its substitute shader; diagFetch: GXV_DIAG_FETCHONLY's)
    uint8_t pixSavedBlend[4] = {};
    float pixAlphaLo = 0.0f, pixAlphaHi = 1.0f;
    uint8_t pixTexK = 0, pixTexN = 0; // (GXV_PIX_CENSUS=2: the texture maps the shader reads, and those with a channel range)
    uint16_t pixFs = 0;
} cur;

uint32_t g_blockOff[UB_COUNT] = {kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock};
uint32_t g_blockLen[UB_COUNT] = {}; // floats of the block at g_blockOff
// The highest position-matrix row the vertices of the next EmitDraw / EmitStaticDraw use
// (their PNMTXIDX), 0xFF: not known. Decides between the short and the full matrix palette.
uint32_t g_pendingPosMtxMax = 0xFF;
bool g_blocksStale = true; // after a flush nothing is in the arena any more
// prewarm24 (GXV_PREWARM, the warm-up scope: WarmScopeBegin below): while the game draws between PORT_GX_MARK_WARM_BEGIN and _END, a
// resolution builds its uniform blocks into this scratch arena (never into the frame's, never on the finishing thread), and no draw is
// recorded: each draw's program variant is queued for the back end instead.
bool g_warmScope = false;
// precomp26 (GXV_STAGE_ENUM): true only while StageEnumDrawMarker resolves its dry draw inside a warm-up scope: no texture is looked up or uploaded for it, and its variants
// go to the enumeration inbox (EnumNote) instead of the back end's warm-up queue and the shader list file.
// layoutwarm24 (GXV_LAYOUTWARM_LOG): between PORT_GX_MARK_LAYOUT_BEGIN and _END (BP 0xFF kinds 19 / 20, the layout's census id in bits 8-23) the record
// sink counts the layout's draws and their program variants (LwCensusDraw below); the markers' end logs what the layout's first draw in play cost.
bool g_lwCensus = false;
void LwCensusDraw(uint16_t vsId, uint16_t fsId, uint32_t layoutMask, uint32_t blendKey);
UniArena g_warmUni;
uint32_t g_warmBlockOff[UB_COUNT] = {kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock, kNoBlock};
uint32_t g_warmBlockLen[UB_COUNT] = {};

size_t g_pendingVtxStart = 0;
int g_lastDraw = -1; // index of the draw that may still be merged into
uint32_t g_lastRecSerial = 0; // Resolved::stateSerial of the state that draw was recorded (or last merged) with

inline bool EfbHasAlpha() { return (g.bp[0x43] & 7) == 1; }
inline bool DstAlphaZeroEnabled() {
    // Zero-only blended destination alpha; opt-in, read once, default off.
    const bool enabled = VGX_KNOB(EnvInt("GXV_DSTALPHA_ZERO", 0) == 1) != 0;
    return enabled;
}
// GXV_FIX_DSTA (default 0): GXSetDstAlpha(TRUE, a) with blending or GX_BM_SUBTRACT writes the constant a to the framebuffer
// alpha for every constant (GXV_DSTALPHA_ZERO covers only a = 0 and needs a colour write), see vgx_fixes.h PlanDstAlpha. Galaxy's volume
// lights, search light and dark mask use it as a mask plane. 1 includes what GXV_DSTALPHA_ZERO does.
inline bool FixDstA() {
    const bool on = VGX_KNOB(EnvInt("GXV_FIX_DSTA", 0) != 0) != 0;
    return on;
}
// GXV_FIX_LOGICOP (default 0): GX logic-op blend (BP 0x41 bit 1) uses blend factors that reproduce the op on 0/255 channels
// (vgx_fixes.h LogicOpPlan) instead of the table in LegacyLogicOpFactors, which is right for 7 of the 16 ops.
inline bool FixLogicOp() {
    const bool on = VGX_KNOB(EnvInt("GXV_FIX_LOGICOP", 0) != 0) != 0;
    return on;
}
// GXV_FIX_TEXOFF (default 0): a TEV stage that reads the texture colour while its texture is switched off in BP 0x28.. reads white
// (as GX does) instead of whatever an earlier stage sampled.
inline bool FixTexOff() {
    const bool on = VGX_KNOB(EnvInt("GXV_FIX_TEXOFF", 0) != 0) != 0;
    return on;
}
uint8_t MapSrcFactor(uint32_t f, bool hasAlpha);
uint8_t MapDstFactor(uint32_t f, bool hasAlpha);
inline fixes::DstAlphaPlan DstAlphaPlanNow() {
    const uint32_t cm0 = g.bp[0x41];
    const bool hasAlpha = EfbHasAlpha();
    return fixes::PlanDstAlpha(bits(g.bp[0x42], 8, 1) != 0, bits(g.bp[0x42], 0, 8), bits(cm0, 4, 1) != 0 && hasAlpha, bits(cm0, 3, 1) != 0,
                               bits(cm0, 0, 1) != 0, bits(cm0, 11, 1) != 0, MapSrcFactor(bits(cm0, 8, 3), hasAlpha), MapDstFactor(bits(cm0, 5, 3), hasAlpha));
}

uint8_t SwapTable(int t) {
    const uint32_t rg = g.bp[0xF6 + 2 * t], ba = g.bp[0xF7 + 2 * t];
    return static_cast<uint8_t>((rg & 3) | (((rg >> 2) & 3) << 2) | ((ba & 3) << 4) | (((ba >> 2) & 3) << 6));
}

uint64_t g_tevDualKeys = 0; // (GXV_TEVIN_PREWRAP: fragment keys built with a wrapped-copy row; tests)
// fs24: GXV_FS_HALF_FOG draws (a block made for a kFogHalfBit key; wide: |bs| > 4096, FogHalfLanes) and GXV_FS_NOCOLOR draws / variants made ("fogfs24:" line)
std::atomic<uint64_t> g_fogHalfDraws{0}, g_fogHalfWide{0}, g_noColorDraws{0}, g_noColorMade{0};
uint64_t g_tevInCanonKeys = 0; // (GXV_TEVIN_CANON: key builds left unmarked although a register was out of range; tests)
// GXV_IND_FLAT (vgx_indflat.h; stage key ind_flat): 0 (default) off; 1 simple warps of blended draws read the plain coordinate; 2 all draws.
int IndFlatMode() { return static_cast<int>(PORT_STAGE_KNOB(int32_t, std::clamp<long>(port::glow26::IndFlatFloor(port::glow26::ClampLevel(port::stageknob::Int("GXV_GLOW26", 0)), static_cast<int>(port::stageknob::Int("GXV_IND_FLAT", 0))), 0, 2))); } // (glow26: level 1+ raises it to 1)
inline uint64_t KeyHashNow() { return g.keyHash ^ indflat::Salt(IndFlatMode()); } // (the memo's hash: a mode change cannot return another mode's shader id)
// GXV_LODBIAS_SAMPLER: the texmaps a key reads by a stage and by an indirect stage.
struct LodSamplerMasks { uint8_t stage = 0, ind = 0; };
static LodSamplerMasks LodSamplerMasksOf(const FsKey& k) {
    LodSamplerMasks m;
    for (int s = 0; s < k.numStages; ++s)
        if (k.stage[s].texEnable) m.stage |= static_cast<uint8_t>(1u << (k.stage[s].texMap & 7u));
    for (int i = 0; i < k.numIndStages; ++i) m.ind |= static_cast<uint8_t>(1u << (k.ind[i].texMap & 7u));
    return m;
}
// Counters of the front end's view (the back end counts the draws: vgx_backend_gl.cpp "lodbias-sampler:"); the key built and the binding made must agree.
std::atomic<uint64_t> g_lodSamplerInconsistent{0};
} // namespace (reopened below)
int EfbPixelFormatNow() { return static_cast<int>(g.bp[0x43] & 7u); } // (GXV_MASKFULL's census: 0 RGB8 + Z24, 1 RGBA6 + Z24)
uint64_t LodSamplerInconsistent() { return g_lodSamplerInconsistent.load(std::memory_order_relaxed); }
namespace {
// What each used texmap of cur.fs gets: the binding's lodSteps (a moved unit) and lodWhy (the census class), into cur.tex[] after it was copied from the slots.
// A stage that is not biased gives 0 / 0. In mode 1 the key already lost the shader bias of every moved stage; the classification is made again from the
// same registers and must agree (lodsampler::Classify is a function of them): a unit the key still biases must not classify as moved, and one it does not
// must.
static void LodSamplerFill(const FsKey& fs, TexBind* tex) {
    const int mode = LodSamplerMode();
    if (__builtin_expect(mode == 0, 1)) return;
    const LodSamplerMasks lm = LodSamplerMasksOf(fs);
    int b32[8];
    LodBiases(g.bp, b32);
    for (unsigned m = 0; m < 8; ++m) {
        if (!(lm.stage & (1u << m))) continue;
        const lodsampler::Unit u = lodsampler::Classify(m, b32, lm.stage, lm.ind, LodSamplerCfg());
        tex[m].lodWhy = u.why;
        tex[m].lodSteps = 0;
        if (mode != 1 || u.why != lodsampler::kMoved) continue;
        // (every stage that reads the unit has lost its bias, or none has: they were classified together)
        bool keyMoved = true;
        for (int s = 0; s < fs.numStages; ++s)
            if (fs.stage[s].texEnable && (fs.stage[s].texMap & 7u) == m && fs.stage[s].lodBias) keyMoved = false;
        if (keyMoved) tex[m].lodSteps = u.steps;
        else g_lodSamplerInconsistent.fetch_add(1, std::memory_order_relaxed);
    }
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
            if ((bits(ind, 9, 4) != 0 || bits(ind, 4, 3) != 0 || bits(ind, 13, 6) != 0 || bits(ind, 20, 1) != 0 ||
                 bits(ind, 7, 2) != 0) &&
                !indflat::Eligible(IndFlatMode(), ind, bits(g.bp[0x41], 0, 1) || bits(g.bp[0x41], 11, 1))) {
                st.ind = ind;
                anyInd = true;
            } else if ((bits(ind, 9, 4) != 0 || bits(ind, 4, 3) != 0 || bits(ind, 13, 6) != 0 || bits(ind, 20, 1) != 0 || bits(ind, 7, 2) != 0)) {
                port::glow26::g_count.indFlatStages.fetch_add(1, std::memory_order_relaxed); // (compfix26: the stage had an indirect read and GXV_IND_FLAT left it direct)
            }
        }
        if (usesTex || st.ind) st.texCoord = static_cast<uint8_t>(bits(tref, 3, 3));
        if (usesTex && bits(tref, 6, 1)) {
            st.texEnable = 1;
            st.texMap = static_cast<uint8_t>(bits(tref, 0, 3));
            st.texSwap = SwapTable(bits(teva, 2, 2));
            // GXV_FIX_LODBIAS: the unit mip-maps with a non-zero LOD bias (mode0 / mode1 are key registers while the switch is on: vgx_state.cpp)
            if (FixLodBias() && fixes::LodBiasOf(g.bp[fixes::TexMode0Reg(st.texMap)], g.bp[fixes::TexMode1Reg(st.texMap)]) != 0.0f) st.lodBias = 1;
        } else if (usesTex && FixTexOff()) {
            st.texOff = 1;
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
    // GXV_LODBIAS_SAMPLER=1 (vgx_lodsampler.h): a stage whose unit's bias the sampler can give exactly loses its shader bias (the key is then the one of a
    // unit without a bias: the shader has no bias read and no bias uniform; the bias travels in the draw's texture binding, LodSamplerFill below).
    // =2 leaves the keys alone (LodSamplerFill only counts).
    if (__builtin_expect(LodSamplerMode() == 1, 0)) {
        LodSamplerMasks lm = LodSamplerMasksOf(k);
        int b32[8];
        LodBiases(g.bp, b32);
        for (int s = 0; s < k.numStages; ++s) {
            FsKey::Stage& st = k.stage[s];
            if (st.texEnable && st.lodBias && lodsampler::Classify(st.texMap & 7u, b32, lm.stage, lm.ind, LodSamplerCfg()).why == lodsampler::kMoved) st.lodBias = 0;
        }
    }
    const uint32_t ac = g.bp[0xF3];
    const uint32_t c0 = bits(ac, 16, 3), c1 = bits(ac, 19, 3), op = bits(ac, 22, 2);
    // Fold the cases that always pass.
    bool pass = (op == 0 && c0 == 7 && c1 == 7) || (op == 1 && (c0 == 7 || c1 == 7)) || (op == 3 && c0 == 7 && c1 == 7) ||
                      (op == 2 && ((c0 == 7 && c1 == 0) || (c0 == 0 && c1 == 7)));
    // GXV_NODISCARD: also the tests the references make constant (GEQUAL 0, LEQUAL 255, ...): no alpha test, no `discard`
    // (the references are in BP 0xF3, a key register, so a change of them builds the key again)
    if (!pass && NoDiscard()) pass = AlphaTestAlwaysPasses(c0, static_cast<int>(bits(ac, 0, 8)), op, c1, static_cast<int>(bits(ac, 8, 8)));
    if (!pass) {
        k.alphaTestOn = 1;
        k.alphaTest = static_cast<uint8_t>(c0 | (c1 << 3) | (op << 6));
    }
    const uint32_t fsel = bits(g.bp[0xF1], 21, 3);
    if (fsel) k.fog = static_cast<uint8_t>(fsel | (bits(g.bp[0xF1], 20, 1) << 3));
    // GXV_FOGVTX: linear fog from the vertex shader (not with a Z-texture replacing the depth: the factor is made from the
    // depth range, which that draw sets to one value)
    if (k.fog && bits(g.bp[0xF5], 2, 2) != 2 && FogVtxEligible(k.fog)) k.fog |= kFogVtxBit;
    const uint32_t zop = bits(g.bp[0xF5], 2, 2);
    if (zop == 1) {
        stats::Note("z-texture (GXSetZTexture GX_ZT_ADD) requested: NOT implemented");
    } // (GX_ZT_REPLACE: BuildRenderState)
    for (int i = 0; i < k.numTexGens; ++i)
        if (bits(g.xfReg[0x40 + i], 1, 1)) k.texProjMask |= static_cast<uint8_t>(1u << i);
    const uint32_t cm0 = g.bp[0x41];
    const bool blending = bits(cm0, 0, 1) || bits(cm0, 11, 1);
    if (FixDstA()) {
        const fixes::DstAlphaPlan p = DstAlphaPlanNow();
        k.dstAlpha = p.fsConst ? 1 : 0;
        if (p.unresolved) stats::Note("GXSetDstAlpha constant with a source-alpha blend factor: constant alpha NOT written (needs a second pass)");
    } else if (bits(g.bp[0x42], 8, 1) && EfbHasAlpha() && bits(cm0, 4, 1)) {
        if (!blending) k.dstAlpha = 1;
        else if (!DstAlphaZeroEnabled() || bits(g.bp[0x42], 0, 8) != 0)
            stats::Note("GXSetDstAlpha together with blending: constant alpha NOT written (needs a second pass)");
    }
    if (FixLogicOp() && !blending && bits(cm0, 1, 1) && bits(cm0, 12, 4) != 3) k.logicSrc = fixes::LogicOpPlan(bits(cm0, 12, 4)).src;
    // GXV_TEVIN_CANON (shader23): State::tevOor is the mask of the registers outside 0..255; a key whose result the mark cannot change (no
    // stage reads such a register's initial value, and every wrapped read would return its argument: FsTevInNeedsMark) stays unmarked:
    // it is then the key of the same state with every register in range (no new shader).
    bool oor = g.tevOor != 0;
    if (oor && TevInCanonMode() && !FsTevInNeedsMark(k, g.tevOor, FixTevIn())) {
        oor = false;
        ++g_tevInCanonKeys;
    }
    if (FixTevIn() && oor) {
        k.inWrap = 1; // (GXV_FIX_TEVIN: a TEV register outside 0..255; State::tevOor)
        if (TevInCanonMode() == 2) k.setPreWrap(FsTevInPreWrapMask(k)); // (registers read only as A / B / C: uploaded wrapped, BuildTevC)
        if (TevInCanonMode() == 2 && TevInPrewrapMode()) { // (GXV_TEVIN_PREWRAP: registers also read as D: a wrapped copy row)
            k.setDualWrap(FsTevInDualMask(k));
            if (k.dualWrap()) ++g_tevDualKeys;
        }
    }
    // GXV_FS_HALF=1 / 3 without GXV_FIX_TEVIN: a register outside 0..255 only matters to a key the half precision analysis would take (it assumes
    // registers in 0..1); that one is marked (bit 1: no wrap code, not half) so that it gets the float shader. Every other key is unchanged.
    else if (oor && TevOorTracked() && FsHalfEligible(k)) k.inWrap = 2;
    // GXV_FS_TRIM: its range analysis takes the registers as 0..255 in a key without the mark; every key of a state with one outside is marked
    // (bit 1: no wrap code, the float shader, as above).
    else if (oor && FsTrimMode()) k.inWrap = 2;
    // GXV_FS_HALF_FOG (fs24): a vertex-fog key the half analysis takes once its fog parameters travel as half-exact lanes (FogHalfLanes, BuildTevC) is marked
    // kFogHalfBit; a key it does not take (the TEV part is over the bound, indirect, ...) stays as it was: the float shader. Not with a register outside
    // 0..255 (the analysis assumes none; the mark above is made only for a key that is half without fog). Decided here, from the key alone.
    if (FsHalfFogMode() && (k.fog & kFogVtxBit) && !oor && !k.inWrap) {
        k.fog |= kFogHalfBit;
        if (!FsHalfEligible(k)) k.fog = static_cast<uint8_t>(k.fog & ~kFogHalfBit);
    }
}

void BuildVsKey(VsKey& k, uint8_t numTexGens, uint8_t palCls, uint8_t fsFog) {
    std::memset(&k, 0, sizeof k);
    k.numTexGens = numTexGens;
    if (fsFog & kFogVtxBit) k.fogVtx = static_cast<uint8_t>((fsFog & 8) ? 3 : 1); // (the fragment key's fog bits: GXV_FOGVTX)
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
    if (k.perVtxPos && palCls) {
        bool texFromPal = false;
        for (int i = 0; i < numTexGens; ++i) texFromPal |= k.tg[i].perVtxMtx != 0;
        if (!texFromPal) k.perVtxPos = palCls == 2 ? 3 : 2;
    }
    // GXV_VS_TRIM: the attenuation facts of the lights this key's lit channels use (State::ltCos1 / ltDist1, part of the key hash); a key
    // without a lit channel is left as it was.
    if (VsTrimTracked()) {
        uint8_t lit = 0;
        for (int i = 0; i < 4; ++i)
            if ((i & 1) < k.numChans && k.chan[i].enable) lit |= k.chan[i].lightMask;
        if (lit) k.setLtTrim(static_cast<uint8_t>(g.ltCos1 & lit), static_cast<uint8_t>(g.ltDist1 & lit));
    }
    // GXV_VS_LTSKIP (vertex25): a lit key's vertex shader skips the loop body of a light whose colour is exactly 0
    if (LtSkipOn()) {
        for (int i = 0; i < 4; ++i)
            if ((i & 1) < k.numChans && k.chan[i].enable && k.chan[i].lightMask) { k.setLtSkip(); break; }
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
        // (same table Dolphin uses for back ends without logic op support; GXV_FIX_LOGICOP: a table that is exact on 0/255 channels).
        const uint32_t op = bits(cm0, 12, 4);
        if (op != 3) {
            rs.blend = 1;
            if (FixLogicOp()) {
                const fixes::LogicPlan lp = fixes::LogicOpPlan(op);
                rs.blendSrc = lp.sf; rs.blendDst = lp.df;
            } else {
                fixes::LegacyLogicOpFactors(op, rs.blendSrc, rs.blendDst);
            }
            stats::Note("logic-op blend mode used (approximated with blend factors)");
        }
    }
    rs.colorWrite = static_cast<uint8_t>(bits(cm0, 3, 1));
    rs.alphaWrite = static_cast<uint8_t>(bits(cm0, 4, 1) && hasAlpha);
    if (FixDstA()) {
        const fixes::DstAlphaPlan p = DstAlphaPlanNow();
        if (p.dropBlend) { rs.blend = 0; rs.blendSrc = BF_ONE; rs.blendDst = BF_ZERO; rs.blendEq = BE_ADD; }
        rs.dstAlphaZero = p.rsMode;
    } else if ((bits(cm0, 0, 1) || bits(cm0, 11, 1)) && rs.alphaWrite && bits(g.bp[0x42], 8, 1) &&
        bits(g.bp[0x42], 0, 8) == 0 && DstAlphaZeroEnabled()) {
        rs.dstAlphaZero = 1;
        // ONE/ZERO is normally unblended, but its alpha still needs the override.
        rs.blend = 1;
    }
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

// GXV_RSMEMO (default 1; 0: off): BuildRenderState for the bits of the registers it reads, as the last time they were these. Materials
// differ in their TEV set-up far more often than in their blend, depth, cull and scissor: most material switches find the render state
// of the one before. Not kept: a state that comes from a Z texture's contents (GX_ZT_REPLACE) or noted a logic-op blend (stats::Note).
bool RsMemoDraw() {
    const bool on = VGX_KNOB(EnvInt("GXV_RSMEMO", 1) != 0) != 0;
    return on;
}
struct RsMemo {
    bool valid = false;
    uint32_t in[16] = {};
    RenderState rs;
} g_rsMemo;
void BuildRenderStateMemo(RenderState& rs) {
    if (!RsMemoDraw()) { BuildRenderState(rs); return; }
    const uint32_t in[16] = {g.bp[0x41] & 0xFFFFu, g.bp[0x40] & 0x1Fu, g.bp[0x00] & 0xC000u, g.bp[0x43] & 7u, g.bp[0x42] & 0x1FFu, g.bp[0x22] & 0xFFu,
                             g.bp[0x59] & 0xFFFFFu, g.bp[0x20] & 0x7FF7FFu, g.bp[0x21] & 0x7FF7FFu, g.xfReg[0x1A], g.xfReg[0x1B], g.xfReg[0x1C],
                             g.xfReg[0x1D], g.xfReg[0x1E], g.xfReg[0x1F], g.bp[0xF5] & 0xCu};
    RsMemo& m = g_rsMemo;
    if (m.valid && SameWords(m.in, in, 16)) {
        rs = m.rs;
        return;
    }
    BuildRenderState(rs);
    const uint32_t cm0 = in[0];
    const bool logicOp = !bits(cm0, 11, 1) && !bits(cm0, 0, 1) && bits(cm0, 1, 1) && bits(cm0, 12, 4) != 3;
    m.valid = bits(in[15], 2, 2) != 2 && !logicOp;
    if (m.valid) {
        CopyWords(m.in, in, 16);
        m.rs = rs;
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

// ---- GXV_TEXBAKE: the texture matrix of a CPU-transformed draw goes into its texture coordinates
// The game's particles (JPA) are drawn one quad at a time. Their position matrix is absorbed by the soft transform (GXV_SOFTXFORM)
// and their TEV colours by register folding (GXV_FOLDREGS), but a particle with a texture animation of its own
// (JPALoadCalcTexCrdMtxAnm: scroll / scale / rotation by the particle's age) loads TEXMTX0 before every quad, so every quad had a
// texture-matrix block of its own and was a draw call of its own ("uniform-blocks" in the merge census). With the switch on, a
// CPU-transformed draw's texture coordinate that comes from a texture coordinate attribute through a 2x4 matrix (texgen type 0,
// source TEX0..7, no per-vertex matrix index, not projective, and no other texgen of the key reads the same attribute) is
// transformed here, as the vertex shader does it:
//     s' = (m0 s + m2) + (m1 t + m3),  t' = (m4 s + m6) + (m5 t + m7)      (srcv = (s, t, 1, 1): the shader's dot products)
// and the block holds the identity rows for that texgen, so the shader computes dot((1,0,0,0), (s',t',1,1)) = s' (exact: products
// with 1 and 0, sums with 0). The block no longer depends on the texture matrix, and consecutive particles merge into one draw.
// What may not be exact: the GPU's own dot product may round differently from the CPU's (fused or not, another order) in the last
// bit of a coordinate (2^-24 of the texture at coordinates near 1). The host test (tests/vgx_texbake_gl.cpp) draws identical
// pictures on Mesa, with exactly representable matrices and with arbitrary ones.
// The post-transform matrix (dual texgen) and its normalisation stay in the shader, after the matrix, as before.
//   GXV_TEXBAKE=0 off (every block and vertex as before);  =1 on.  Read once.
bool TexBakeOn() {
    const bool on = VGX_KNOB(EnvInt("GXV_TEXBAKE", 0) != 0) != 0;
    return on;
}
// The texgens of `vs` whose matrix is baked when a draw with this key is CPU-transformed (a pure function of the key: the
// matrix block (BuildMtxC, MtxReads, on either thread) and the vertices (EmitDraw) decide the same).
uint8_t TexBakeMask(const VsKey& vs) {
    if (!TexBakeOn() || vs.perVtxPos || vs.lpMode()) return 0;
    uint8_t m = 0;
    for (int i = 0; i < vs.numTexGens && i < 8; ++i) {
        const VsKey::TexGen& t = vs.tg[i];
        if (t.type != 0 || t.src < 5 || t.src > 12 || t.perVtxMtx || t.proj) continue;
        bool shared = false;
        for (int j = 0; j < vs.numTexGens && j < 8; ++j)
            if (j != i && vs.tg[j].type < 2 && vs.tg[j].src == t.src) shared = true;
        if (!shared) m |= static_cast<uint8_t>(1u << i);
    }
    return m;
}
// (GXV_FE_SPLIT=1 with GXV_WORKLOAD_PROBE=1: the "uniform-blocks" merge failures by the blocks that differ, for the texbake line
// FrontPresent writes every 300 pictures; on the thread that finishes the records)
std::atomic<uint32_t> g_blockMiss[4]; // only the texture-matrix block differs; only the model-view block; only the TEV block; other
void NoteBlockMiss(const uint32_t* a, const uint32_t* b) {
    uint32_t m = 0;
    for (int k = 0; k < UB_COUNT; ++k)
        if (a[k] != b[k]) m |= 1u << k;
    const int slot = m == (1u << UB_TEXM) ? 0 : m == (1u << UB_MV) ? 1 : m == (1u << UB_TEV) ? 2 : 3;
    g_blockMiss[slot].fetch_add(1, std::memory_order_relaxed);
}
// The texture-matrix row of texgen i (as BuildMtxC reads it).
inline uint32_t TexGenRow(const State& s, int i) { return i < 4 ? bits(s.matIdxA, 6 + 6 * i, 6) : bits(s.matIdxB, 6 * (i - 4), 6); }
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
    const uint32_t bake = soft ? TexBakeMask(vs) : 0u; // (GXV_TEXBAKE: these texgens' matrices are in the coordinates)
    static const float kIdentRows[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    for (int i = 0; i < vs.numTexGens; ++i) {
        const uint32_t ti = i < 4 ? bits(g.matIdxA, 6 + 6 * i, 6) : bits(g.matIdxB, 6 * (i - 4), 6);
        const uint32_t po = g.xfReg[0x50 + i] & 0x3F;
        const bool baked = ((bake >> i) & 1u) != 0;
        if (baked) std::memcpy(&tm[i * 12], kIdentRows, sizeof kIdentRows); // (the matrix rows are not read: not in `rows`)
        else use(ti);
        for (int r = 0; r < 3; ++r) {
            if (!baked) std::memcpy(&tm[i * 12 + r * 4], g.xfPos[ti + r < 64 ? ti + r : 63], 16);
            if (!pack || anyPost) std::memcpy(&tm[96 + i * 12 + r * 4], g.xfPost[po + r < 64 ? po + r : 63], 16);
        }
    }
    if (tmLen) PushBlock(c, UB_TEXM, tm, tmLen);
    else c.blockOff[UB_TEXM] = kNoBlock;
    bool pal = vs.perVtxPos != 0;
    for (int i = 0; i < vs.numTexGens; ++i) pal |= vs.tg[i].perVtxMtx != 0;
    if (pal) {
        float pb[384];
        if (vs.perVtxPos == 2 || vs.perVtxPos == 3) {
            // Short palette: position rows 0..K-1, then normal rows 0..K-1 (K 30; the small palette, perVtxPos 3: 12).
            const int K = PalRowsOfKey(vs.perVtxPos);
            std::memcpy(pb, g.xfPos, static_cast<size_t>(K) * 4 * sizeof(float));
            for (int r = 0; r < K; ++r) {
                pb[K * 4 + r * 4] = g.xfNrm[r][0]; pb[K * 4 + 1 + r * 4] = g.xfNrm[r][1]; pb[K * 4 + 2 + r * 4] = g.xfNrm[r][2]; pb[K * 4 + 3 + r * 4] = 0;
            }
            PushBlock(c, UB_PAL, pb, static_cast<uint32_t>(K) * 8);
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
    const uint32_t bake = soft ? TexBakeMask(vs) : 0u; // (GXV_TEXBAKE: as BuildMtxC, those rows are not read)
    for (int i = 0; i < vs.numTexGens; ++i) {
        const uint32_t ti = i < 4 ? bits(g.matIdxA, 6 + 6 * i, 6) : bits(g.matIdxB, 6 * (i - 4), 6);
        const uint32_t po = g.xfReg[0x50 + i] & 0x3F;
        if (!((bake >> i) & 1u)) {
            used |= three(ti);
            pos |= three(ti);
        }
        if (!pack || anyPost) post |= three(po);
    }
    bool pal = vs.perVtxPos != 0;
    for (int i = 0; i < vs.numTexGens; ++i) pal |= vs.tg[i].perVtxMtx != 0;
    if (pal) {
        if (vs.perVtxPos == 2 || vs.perVtxPos == 3) {
            const int K = PalRowsOfKey(vs.perVtxPos);
            pos |= (1ull << K) - 1;
            nrm |= (1u << K) - 1;
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
    const bool on = VGX_KNOB(EnvInt("GXV_LIGHTNORM", 1) != 0) != 0;
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
uint64_t TevInPrewrapKeys() { return g_tevDualKeys; } // (tests)
void split::BuildTevC(BlockCtx& bc, const State& g, const FsKey& fs, const uint16_t texW[8], const uint16_t texH[8], bool fold) {
    float tb[128]; // (28 rows, and the four of GXV_TEVIN_PREWRAP's wrapped copies)
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
            // (GXV_TEVIN_CANON=2, FsKey::preWrap: the register is only read as A / B / C: its low byte, as GX's input and vgx_w1 give it)
            if (((fs.preWrap() >> (c < 3 ? i : i + 4)) & 1u) && (fs.inWrap & 1) && !(fold && (i == 1 || i == 2)))
                tb[TEV_REG + i * 4 + c] = static_cast<float>(g.tevReg[i][c] & 255) / 255.0f;
            tb[TEV_KONST + i * 4 + c] = static_cast<float>(g.tevKonst[i][c] & 0xFF) / 255.0f;
        }
    if (fs.dualWrap()) { // (GXV_TEVIN_PREWRAP: register r's four components wrapped to their low byte, (v & 255) / 255, as vgx_w1 / GX's inputs A, B, C give them)
        const uint32_t wb = static_cast<uint32_t>(FsTevBase(fs)) * 4;
        static const bool brk = diag::Int("GXV_PREWRAP_BREAK", 0) == 1; // (desktop diagnostic: an unwrapped copy, wrong on purpose: the host test's control)
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                tb[wb + r * 4 + c] = (((fs.dualWrap() >> r) & 1u) && !(fold && (r == 1 || r == 2)))
                                         ? static_cast<float>(brk ? g.tevReg[r][c] : (g.tevReg[r][c] & 255)) / 255.0f
                                         : 0.0f;
    }
    tb[TEV_MISC + 0] = static_cast<float>(bits(g.bp[0xF3], 0, 8));
    tb[TEV_MISC + 1] = static_cast<float>(bits(g.bp[0xF3], 8, 8));
    tb[TEV_MISC + 2] = static_cast<float>(bits(g.bp[0x42], 0, 8)) / 255.0f;
    tb[TEV_MISC + 3] = static_cast<float>(g.bp[0xF4]) / 16777215.0f;
    if (FsAtFastKey(fs)) { // GXV_FS_ATFAST: the cut-out's thresholds in alpha units, in the fog row (a key without fog has none; FsTevCount gives it 10 rows)
        float thr[2];
        AtFastValues(fs.alphaTest, tb[TEV_MISC + 0], tb[TEV_MISC + 1], thr);
        tb[TEV_FOG + 0] = thr[0];
        tb[TEV_FOG + 1] = thr[1];
    }
    if (fs.fog) {
        const float a = FogFloat(g.bp[0xEE]);
        const float bm = static_cast<float>(g.bp[0xEF]);
        const float sh = std::ldexp(1.0f, static_cast<int>(bits(g.bp[0xF0], 0, 5)));
        if (fs.fog & 8) { tb[TEV_FOG + 0] = a; tb[TEV_FOG + 1] = 0; }
        else { tb[TEV_FOG + 0] = a * sh; tb[TEV_FOG + 1] = bm * sh / 16777216.0f; }
        tb[TEV_FOG + 2] = FogFloat(g.bp[0xF1]);
        if (fs.fog & kFogVtxBit) {
            // GXV_FOGVTX: the fragment shader gets c1 (x) and c0 (w) of clamp(c1 * v_fog + c0): FogVtxCoeffs. The depth range is
            // the one BuildRenderState gives glDepthRange; the projection's parameters travel with the projection block (split:
            // OP_PROJ), the viewport's z scale and offset with this one (split: OP_TEV).
            const float sz = u2f(g.xfReg[0x1C]), oz = u2f(g.xfReg[0x1F]);
            float zf = oz / 16777215.0f, zn = (oz - sz) / 16777215.0f;
            if (zn < 0) zn = 0;
            if (zf > 1) zf = 1;
            const FogVtxCoef c = FogVtxCoeffs(tb[TEV_FOG + 0], tb[TEV_FOG + 1], tb[TEV_FOG + 2], u2f(g.xfReg[0x24]), u2f(g.xfReg[0x25]), zn, zf,
                                              (fs.fog & 8) != 0);
            tb[TEV_FOG + 0] = c.c1;
            tb[TEV_FOG + 1] = 0.0f;
            tb[TEV_FOG + 2] = 0.0f;
            tb[TEV_FOG + 3] = c.c0;
            if (fs.fog & kFogHalfBit) { // GXV_FS_HALF_FOG: row 11 sInv, c1s, bs hi, bs lo, and row 10's w = the flat factor (FogHalfLanes; row 9 stays for the ubershader)
                const FogHalfLane l = FogHalfLanes(c.c1, c.c0);
                tb[TEV_INDMTX + 0] = l.sInv;
                tb[TEV_INDMTX + 1] = l.c1s;
                tb[TEV_INDMTX + 2] = l.bHi;
                tb[TEV_INDMTX + 3] = l.bLo;
                tb[TEV_FOGCOL + 3] = l.k;
                g_fogHalfDraws.fetch_add(1, std::memory_order_relaxed);
                if (l.wide) g_fogHalfWide.fetch_add(1, std::memory_order_relaxed);
            }
        }
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
        // GXV_IND_PRECOMP: (1 / tsz, c) of the marked stage in the block's last vec4 (u_tev[27])
        if (fs.indPre() && fs.indPre() <= fs.numStages) {
            const unsigned m = fs.stage[fs.indPre() - 1].texMap & 7u;
            IndPreValues(fs, &tb[TEV_INDMTX], texW[m], texH[m], &tb[TEV_INDPRE]);
        }
    }
    // GXV_FIX_LODBIAS: the bias of each texmap a stage samples with one (the shader reads u_tev[V_LODBIAS ..], FsTevCount gives such a shader 28 vec4)
    for (int s = 0; s < fs.numStages; ++s)
        if (fs.stage[s].lodBias) {
            const unsigned m = fs.stage[s].texMap & 7u;
            tb[TEV_LODBIAS + m] = fixes::LodBiasOf(g.bp[fixes::TexMode0Reg(m)], g.bp[fixes::TexMode1Reg(m)]);
        }
    PushBlock(bc, UB_TEV, tb, tevLen);
}

// Small meshes can be transformed to eye space on the CPU so that consecutive
// draws that differ only in their model-view matrix (grass blades, crowd
// sprites, particles: hundreds per frame in this game) share one uniform set
// and merge into a single draw call. GXV_SOFTXFORM=<max verts>, 0 = off.
uint32_t SoftXformLimit() {
    const uint32_t n = static_cast<uint32_t>(VGX_KNOB(static_cast<uint32_t>(EnvInt("GXV_SOFTXFORM", 128))));
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
    const int neonMode = static_cast<int>(VGX_KNOB(EnvInt("GXV_SOFTNEON", 1)));
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
    const bool on = VGX_KNOB(EnvInt("GXV_FOLDREGS", 1) != 0) != 0;
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
// The shader keys of a draw as a pair (vgx_shadergen.h), right after BuildFsKey / BuildVsKey. Both are pure functions of the two keys
// (so the key memo of GXV_RSCACHE, which holds ids made from them, stays right); with the switches off nothing is changed.
//   GXV_TEXVEC2 (TexVec2Pair): the non-projective texture coordinate varyings become vec2 in both keys.
//   GXV_VSOUT (VsOutPrune): the vertex key marks what its fragment shader never reads. The register-folding variant of the fragment
//   shader (regFromVtx = 3, chosen per draw by values, the same vertex shader) reads both colours whole: a pair that can fold keeps them.
//   GXV_IND_VS (IndVsPair): a texgen that feeds only indirect lookups is written scaled by the vertex shader, and the lookups read it
//   as it is (BP 0x25 / 0x26 are key registers: vgx_state.cpp IsKeyBP, so the memo's hash covers them).
inline void PairKeys(FsKey& fs, VsKey& vs) {
    TexVec2Pair(fs, vs);
    if (IndVsMode()) IndVsPair(fs, vs, g.bp[0x25], g.bp[0x26]);
    if (IndPreMode()) IndPreMark(fs); // (GXV_IND_PRECOMP: a minor change, off by default)
    if (VsOutMode()) VsOutPrune(vs, fs, FoldRegsEnabled() && FoldKeysOk(fs, vs));
}
// GXV_DISCARD_BLEND (DiscardBlend, vgx_shadergen.h): the fragment shader a draw is recorded with, for its render state. A draw whose
// alpha test can fail is drawn with the key's rejectZero variant (failing fragments output vec4(0), no `discard`) when that output
// leaves every value the draw could write as it was:
//   - no depth write (the test is on and the mask set: as the back end applies them);
//   - colour: not written, or blended with an equation that adds or reverse-subtracts the source term (BE_ADD, BE_REVSUB), whose
//     destination factor is 1 for a zero source (ONE, ONE_MINUS_SRC_ALPHA, ONE_MINUS_SRC_COLOR): dst * 1 +/- 0 * sf = dst;
//   - alpha: not written, or blended with the colour's factors (dstAlphaZero 0): the same argument with the source alpha 0.
// That is the result of a killed fragment, exactly (x * 1 = x and 0 * f = 0 for every finite factor; the factors are in 0..1):
// RejectIsNoop (vgx_cmd.h). The key is a different key (its own id, text and pack entry); the variant ids are memoised per
// fragment shader id.
std::vector<uint16_t> g_rejectZeroVar; // per fragment shader id: its rejectZero variant's id + 1 (0: not made yet)
uint64_t g_rejectZeroDraws = 0;        // (resolutions that took a variant: census for tests)
// GXV_FS_ADEAD (shader23 round 2, FsAlphaDeadMode): the same for a draw whose render state uses nothing of the source alpha
// (AlphaOutputUnused: alpha not written, no source-alpha blend factor) and whose key has no alpha test and no destination-alpha constant:
// the key's alphaDead variant (alpha output 0; the compiler drops what of the alpha pipeline the colour does not read). Disjoint from the
// rejectZero variant (that one needs an alpha test). Memoised per fragment shader id.
std::vector<uint16_t> g_alphaDeadVar; // per fragment shader id: its alphaDead variant's id + 1 (0: not made yet)
uint64_t g_alphaDeadDraws = 0;        // (resolutions that took a variant: census for tests)
uint16_t AlphaDeadFs(uint16_t id, const RenderState& rs) {
    if (!FsAlphaDeadMode() || id >= g_fsReg.keys.size()) return id;
    {
        const FsKey& k = g_fsReg.keys[id];
        if (k.alphaTestOn || k.dstAlpha || k.alphaDead() || k.rejectZero() || !AlphaOutputUnused(rs)) return id;
    }
    ++g_alphaDeadDraws;
    if (id < g_alphaDeadVar.size() && g_alphaDeadVar[id]) return static_cast<uint16_t>(g_alphaDeadVar[id] - 1);
    FsKey v = g_fsReg.keys[id]; // (a copy: Lookup may grow the registry)
    v.setAlphaDead(1);
    const uint16_t vid = Lookup(g_fsReg, v, true, GenerateFragmentShader);
    if (g_alphaDeadVar.size() <= id) g_alphaDeadVar.resize(static_cast<size_t>(id) + 1, 0);
    g_alphaDeadVar[id] = static_cast<uint16_t>(vid + 1);
    keys27::SetGeneral(vid, id, keys27::kAlphaDead, v); // (keys27: the general form is the key the variant came from: same u_tev block, same result for this draw)
    return vid;
}
// GXV_FS_NOCOLOR (fs24, FsNoColorMode): a draw whose render state writes no colour (colour mask off) and whose blend factors do not read the source colour
// (SRC_COLOR / ONE_MINUS_SRC_COLOR: a blend of the alpha channel with them reads the source alpha in GL, but the test is kept plain) is drawn with the key's
// colorDead variant: colour 0, no fog; the compiler drops the colour pipeline. Exact: the colour mask discards every colour the shader outputs, and the alpha and the
// alpha test do not read the colour pipeline (a TEV stage's alpha inputs are alpha channels only). Not for a key with the silhouette fuse (it writes colour through
// its own blend), the alpha prepass (writes 0 already) or a logic-op output. Memoised per fragment shader id.
std::vector<uint16_t> g_colorDeadVar; // per fragment shader id: its colorDead variant's id + 1 (0: not made yet)
uint16_t ColorDeadFs(uint16_t id, const RenderState& rs) {
    if (!FsNoColorMode() || id >= g_fsReg.keys.size() || rs.colorWrite) return id;
    if (rs.blend && (rs.blendSrc == BF_SRC_COLOR || rs.blendSrc == BF_INV_SRC_COLOR || rs.blendDst == BF_SRC_COLOR || rs.blendDst == BF_INV_SRC_COLOR)) return id;
    {
        const FsKey& k = g_fsReg.keys[id];
        if (k.colorDead() || k.silhFuse() || k.preAlpha() || k.logicSrc) return id;
    }
    g_noColorDraws.fetch_add(1, std::memory_order_relaxed);
    if (id < g_colorDeadVar.size() && g_colorDeadVar[id]) return static_cast<uint16_t>(g_colorDeadVar[id] - 1);
    FsKey v = g_fsReg.keys[id]; // (a copy: Lookup may grow the registry)
    v.setColorDead(1);
    const uint16_t vid = Lookup(g_fsReg, v, true, GenerateFragmentShader);
    if (g_colorDeadVar.size() <= id) g_colorDeadVar.resize(static_cast<size_t>(id) + 1, 0);
    g_colorDeadVar[id] = static_cast<uint16_t>(vid + 1);
    g_noColorMade.fetch_add(1, std::memory_order_relaxed);
    keys27::SetGeneral(vid, id, keys27::kColorDead, v); // (keys27: the general form is the key the variant came from: same u_tev block, the same alpha and the same masked colour)
    return vid;
}
uint16_t RejectZeroFs(uint16_t id, const RenderState& rs) {
    id = ColorDeadFs(id, rs); // (GXV_FS_NOCOLOR; with the switch off: id unchanged)
    id = AlphaDeadFs(id, rs); // (GXV_FS_ADEAD; with the switch off: id unchanged)
    if (!DiscardBlend() || id >= g_fsReg.keys.size()) return id;
    {
        const FsKey& k = g_fsReg.keys[id];
        if (!k.alphaTestOn || k.rejectZero() || k.atPass() || !RejectIsNoop(rs)) return id; // (an atPass key has no test text)
    }
    ++g_rejectZeroDraws;
    if (id < g_rejectZeroVar.size() && g_rejectZeroVar[id]) return static_cast<uint16_t>(g_rejectZeroVar[id] - 1);
    FsKey v = g_fsReg.keys[id]; // (a copy: Lookup may grow the registry)
    v.setRejectZero(1);
    const uint16_t vid = Lookup(g_fsReg, v, true, GenerateFragmentShader);
    if (g_rejectZeroVar.size() <= id) g_rejectZeroVar.resize(static_cast<size_t>(id) + 1, 0);
    g_rejectZeroVar[id] = static_cast<uint16_t>(vid + 1);
    keys27::SetGeneral(vid, id, keys27::kRejectZero, v); // (keys27: as AlphaDeadFs)
    return vid;
}
// ================================================================ pix23: value-proven pixel cuts (GXV_PIX_ZERO, GXV_PIX_ATPASS)
// The fragment shader's output is worked out from the draw's own values (vgx_shadergen.h FsValueEval: registers, konst colours, alpha
// references, the textures' channel ranges recorded at upload, the rasterised colours from the vertex key and material colours):
//   GXV_PIX_ZERO=1    a draw that cannot change any pixel is left out, as `cullAll` leaves one out:
//                      - its alpha test fails for every fragment (`discard` everywhere; a rejectZero key writes vec4(0), a no-op there);
//                      - or it writes no depth and its blend leaves colour and alpha as they are because the source term is exactly 0
//                        (alpha exactly 0 under SRC_ALPHA / ONE_MINUS_SRC_ALPHA or SRC_ALPHA / ONE, colour exactly 0 under ONE / ONE,
//                        ...): dst * 1 + 0 = dst for every finite factor, exactly (RejectIsNoop's argument).
//                     Exact: nothing the draw does reaches the target. Saves its vertex work, its fragments and a program switch.
//   GXV_PIX_ATPASS=1  a draw whose alpha test passes for every fragment (e.g. a cut-out material on a texture whose alpha is 255
//                     everywhere) is drawn with the key's atPass variant: the same text without the test and its `discard`, so the
//                     GPU's hidden-surface removal works for it and for what lies behind it. Exact: no fragment was ever killed, and
//                     the variant's text is checked once to be the key's text less the test lines (else the variant is refused).
//                     A new key: its text compiles on first use (a pack refill takes it in).
//   =2 (either): decide and count only ("would"), change nothing.
//   GXV_PIX_ZERO=3    (defaults48) the LEAN way: only the proof from a small draw's own vertex colours (PixVtxRecheck: a layout pane or sprite whose
//                     vertex alpha is 0 under a blend that then leaves the target as it is). Nothing is worked out when a draw's state is resolved (no
//                     gather, no hash, no memo) and no texture's channel range is recorded at upload (the proof takes every texture as 0..255; a
//                     vertex component of exactly 0 times anything is still exactly 0). Why: Vita, build c36c, Honeyhive, same place, 30 pairs twice
//                     (night35 rows 46 / 48 against 47): =1 costs the game thread +0.83 / +0.91 ms a drawn turn (the resolution-time evaluation of
//                     ~320 blended or alpha-tested draws a picture) for -0.3 ms of GPU 3D = -3.2 / -3.5 fps where the game thread is the limit, while
//                     13.0 of the 13.2 draws a picture it leaves out come from the vertex-colour proof. Exact, as =1 (a subset of its decisions).
// Read once. Log: "pix23:" every 10 s while either is on (PixLog).
long PixZeroMode() {
    const long m = VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_PIX_ZERO", 0), 0, 3)));
    return m;
}
long PixAtPassMode() {
    const long m = VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_PIX_ATPASS", 0), 0, 2)));
    return m;
}
// gpu960: GXV_PIX_BLENDOFF (0 off; 1 exact: a blended draw whose source alpha is exactly 1.0 for every fragment, under factors that then make the blend an
// overwrite, is drawn unblended; 2 also MINOR: alpha proven >= 1 - L/255 (L = GXV_PIX_BLENDOFF_LEVELS, default 1.0), at most about L + 1 byte levels
// different where the blend would have mixed in the destination); GXV_PIX_CENSUS (1: the coverage-weighted census of what the two proofs and
// GXV_PIX_ATPASS could take, in a "gpu960:" log line); GXV_GPU_BISECT (diagnostic, NOT exact: 1 opaque textured draws get a fragment shader that outputs the
// rasterised colour only, 2 they read no texture: where does the world's pixel time go).
long PixBlendOffMode() {
    const long m = VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_PIX_BLENDOFF", 0), 0, 2)));
    return m;
}
double PixBlendOffLevels() {
    static const double v = [] { const long t = EnvInt("GXV_PIX_BLENDOFF_LEVELS", 10); return std::clamp<long>(t, 0, 40) / 10.0; }(); // (tenths of a level)
    return v;
}
bool PixCensusOn() {
    const bool on = VGX_KNOB(EnvInt("GXV_PIX_CENSUS", 0) != 0) != 0;
    return on;
}
long PixBisectMode() {
    const long m = VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_GPU_BISECT", 0), 0, 2)));
    return m;
}
inline bool PixAny() { return PixZeroMode() != 0 || PixAtPassMode() != 0 || PixBlendOffMode() != 0 || PixCensusOn() || PixBisectMode() != 0; }
enum : uint8_t { PIXC_TEST = 1, PIXC_TPASS = 2, PIXC_TRAW = 4, PIXC_TFAIL = 8, PIXC_BLEND = 16, PIXC_BONE = 32, PIXC_BNEAR = 64, PIXC_OPAQUE = 128 };
namespace {
struct PixCtr {
    uint64_t pictures = 0, draws = 0, blended = 0, tested = 0;      // draws emitted (after the culls), of them blended / alpha-tested
    uint64_t zeroKill = 0, zeroBlend = 0, zeroIdx = 0;              // GXV_PIX_ZERO (or would): the alpha test kills all / the blend is a no-op
    uint64_t atDraws = 0, atIdx = 0;                                // GXV_PIX_ATPASS (or would)
    uint64_t evals = 0, notOk = 0, atMade = 0, atRefused = 0;       // resolutions evaluated, keys not analysed, variants made / refused
    uint64_t vtxChecks = 0, zeroVtx = 0;                            // draws re-checked with their vertex colours, of them proven no-ops
    uint64_t memoHits = 0;                                          // evaluations answered by the memo (PixEval)
    uint64_t zeroPairKept = 0;                                      // (defaults48) no-op draws drawn all the same because the picture opens a 60 Assist pair
    // gpu960 census: draws, indices and screens (an upper bound: the box / extent of the draw, see PixCover) of each class
    struct Row { uint64_t n = 0; double idx = 0, scr = 0; };
    Row cTest, cTpass, cTraw, cTfail, cBlend, cBone, cBnear, cOpaque;
    uint64_t blendOff = 0, blendOffNear = 0; double blendOffScr = 0;  // GXV_PIX_BLENDOFF: draws drawn unblended (exact / within L), their screens
    uint64_t bisect = 0; double bisectIdx = 0;                       // GXV_GPU_BISECT: draws given the substitute shader, their indices
    uint64_t unmeasured = 0;                                         // census draws with no bound (counted as one screen)
};
PixCtr g_pix, g_pixTotal;
float g_pixFrac = -1.0f; // gpu960: the screen fraction (0..1, an upper bound) of the draw about to be emitted, set by PixCover*; < 0: not known
struct PixTop { uint16_t fs; uint8_t cls; uint32_t n; double scr; float aLo, aHi; };
// (heavyopt25, GXV_PIX_CENSUS=2) one row per (fragment shader, vertex shader, blend factors, depth write): what the blended / alpha-tested draws of a window are, with
// the alpha bounds the proof had (a texture whose channel range is not known leaves 0..1: texK says which of the draw's maps had a range)
struct PixRow { uint16_t fs, vs; uint8_t cls, src, dst, dw, texK, texN; uint32_t n, idx; double scr; float aLo, aHi; };
std::vector<PixRow> g_pixRows;
bool PixCensus2() { static const bool on = EnvInt("GXV_PIX_CENSUS", 0) >= 2; return on; }
std::vector<PixTop> g_pixTop; // the census's near-misses (an alpha test that passes without the filter margin; a blend within a few levels) by fragment shader, this window
std::vector<uint16_t> g_atPassVar; // per fragment shader id: its atPass variant's id + 1 (0: not made yet; 0xFFFF: refused)
bool PixLitDebug() {
    static const bool on = EnvInt("GXV_DBG_LIT", 0) != 0; // (a test switch that whitens lit channels: nothing is proven then)
    return on;
}
// The key's text less the alpha test's lines (the ax / ai lines and the `if (!(...)) discard;` line).
std::string PixStripTest(const std::string& t) {
    std::string r;
    r.reserve(t.size());
    size_t i = 0;
    while (i < t.size()) {
        size_t j = t.find('\n', i);
        if (j == std::string::npos) j = t.size() - 1;
        const std::string line = t.substr(i, j + 1 - i);
        const bool test = line.rfind("  float ax = ", 0) == 0 || line.rfind("  float ai = ", 0) == 0 ||
                          (line.rfind("  if (!(", 0) == 0 && line.find("discard;") != std::string::npos);
        if (!test) r += line;
        i = j + 1;
    }
    return r;
}
// GXV_PIX_ATPASS: the atPass variant of fragment shader `id` (an alpha-tested key), or `id` when refused.
uint16_t AtPassFs(uint16_t id) {
    if (id < g_atPassVar.size() && g_atPassVar[id]) return g_atPassVar[id] == 0xFFFFu ? id : static_cast<uint16_t>(g_atPassVar[id] - 1);
    FsKey v = g_fsReg.keys[id]; // (a copy: Lookup may grow the registry)
    v.setAtPass(1);
    const uint16_t vid = Lookup(g_fsReg, v, true, GenerateFragmentShader);
    const bool same = vid != id && vid < g_fsReg.sources.size() && id < g_fsReg.sources.size() &&
                      PixStripTest(g_fsReg.sources[id]->text) == g_fsReg.sources[vid]->text;
    if (g_atPassVar.size() <= id) g_atPassVar.resize(static_cast<size_t>(id) + 1, 0);
    if (!same) {
        g_atPassVar[id] = 0xFFFFu;
        ++g_pixTotal.atRefused;
        VGX_LOG_N(16, "pix23: atPass variant of fragment shader %u refused: its text is not the key's text less the alpha test\n", id);
        return id;
    }
    g_atPassVar[id] = static_cast<uint16_t>(vid + 1);
    ++g_pixTotal.atMade;
    keys27::SetGeneral(vid, id, keys27::kAtPass, v); // (keys27: the tested key; its text is this text plus the test, checked above)
    return vid;
}
// ---- GXV_ALPHATEST_PREPASS (dread25, vgx_prepass.h): the alpha-tested draw as a depth-only first pass and a colour pass without `discard`.
// The front end only chooses the fragment shader: the record carries the PREPASS key's id (a key that has the test and nothing after it); the back end finds the pair
// (tested key, colour variant) here, draws the passes and may fall back to the tested key (a draw it cannot split). Stage-settable: stage_knob.h "alphatest_prepass".
long PrepassMode() {
    static uint32_t gen = ~0u;
    static long m = 0;
    const uint32_t g2 = port::stageknob::Gen();
    if (g2 != gen) {
        gen = g2;
        m = std::clamp<long>(port::stageknob::Int("GXV_ALPHATEST_PREPASS", 0), 0, 5);
    }
    return m;
}
constexpr uint32_t kPreMax = 8192; // (fragment shader ids at or above this are not split)
std::atomic<uint32_t> g_preMap[kPreMax]; // by prepass key id: ((tested id + 1) << 16) | colour id; 0: not a prepass key
std::atomic<uint32_t> g_preAny{0};
std::vector<uint16_t> g_preVar; // per tested key id: its prepass key's id + 1 (0: not made yet; 0xFFFF: refused)
struct PreCtr { uint64_t draws = 0, madeKeys = 0, refused = 0, why[6] = {}; }; // why: [0] state not plain, [1] unused, [2] unused, [3] key flags (reject-zero, atPass, depth texture, logic op), [4] id too large, [5] refused key
PreCtr g_preCtr;
// The prepass key for fragment shader `id` (tested, not yet a variant of any kind) when the draw's state allows the split; else `id`.
uint16_t PrepassFs(uint16_t id, const RenderState& rs) {
    if (PrepassMode() == 0 || PrepassMode() == 3 || id >= g_fsReg.keys.size()) return id;
    if (id >= kPreMax) { ++g_preCtr.why[4]; return id; }
    const FsKey& k = g_fsReg.keys[id];
    if (!FsHasAlphaTestText(k)) return id; // (not a tested key: nothing to count)
    if (!atpp::StateOk(rs)) { ++g_preCtr.why[0]; return id; }
    if (k.rejectZero() || k.atPass() || k.preAlpha() || k.ztex || k.logicSrc) { ++g_preCtr.why[3]; return id; } // (a folded key is fine: the text up to the test is the same text)
    if (id < g_preVar.size() && g_preVar[id]) {
        if (g_preVar[id] == 0xFFFFu) { ++g_preCtr.why[5]; return id; }
        ++g_preCtr.draws;
        return static_cast<uint16_t>(g_preVar[id] - 1);
    }
    if (g_preVar.size() <= id) g_preVar.resize(static_cast<size_t>(id) + 1, 0);
    const uint16_t cid = AtPassFs(id); // (the colour variant; id itself when refused)
    FsKey v = g_fsReg.keys[id];        // (a copy: Lookup may grow the registry)
    v.setPreAlpha(1);
    const uint16_t pid = Lookup(g_fsReg, v, true, GenerateFragmentShader);
    const bool ok = cid != id && pid != id && pid < kPreMax && pid < g_fsReg.sources.size() && id < g_fsReg.sources.size() && g_fsReg.sources[pid] && g_fsReg.sources[id] &&
                    atpp::TextOk(g_fsReg.sources[id]->text, g_fsReg.sources[pid]->text);
    if (!ok) {
        g_preVar[id] = 0xFFFFu;
        ++g_preCtr.refused;
        VGX_LOG_N(16, "atpp25: prepass key of fragment shader %u refused (colour variant %u, prepass %u): the texts are not the tested text split at its test\n", id, cid, pid);
        return id;
    }
    g_preVar[id] = static_cast<uint16_t>(pid + 1);
    g_preMap[pid].store(((static_cast<uint32_t>(id) + 1u) << 16) | cid, std::memory_order_release);
    g_preAny.store(1, std::memory_order_release);
    ++g_preCtr.madeKeys;
    ++g_preCtr.draws;
    return pid;
}
// The rasterised colours a draw's vertex key makes (v_col0 / v_col1), from the material colours (XF 0x0C / 0x0D) where they are the source.
void PixRas(const VsKey& vs, FsValueIn& in) {
    for (int ch = 0; ch < 2; ++ch) {
        const uint32_t mat = g.xfReg[0x0C + ch];
        const int mb[4] = {static_cast<int>((mat >> 24) & 0xFF), static_cast<int>((mat >> 16) & 0xFF), static_cast<int>((mat >> 8) & 0xFF),
                           static_cast<int>(mat & 0xFF)};
        for (int comp = 0; comp < 4; ++comp) {
            in.rasLo[ch][comp] = 0.0f;
            in.rasHi[ch][comp] = 1.0f;
            in.rasZero[ch][comp] = false;
            if (ch >= vs.numChans || PixLitDebug()) continue; // (the vertex colour attribute itself: 0..1)
            const VsKey::Chan& c = vs.chan[ch + (comp == 3 ? 2 : 0)];
            if (c.matSrcVtx) continue;                         // (the vertex colour times the light: 0..1)
            const float m = mb[comp] / 255.0f;
            in.rasHi[ch][comp] = m;
            in.rasLo[ch][comp] = c.enable ? 0.0f : m;           // (lit: the material colour times a light sum clamped to 0..1)
            in.rasZero[ch][comp] = mb[comp] == 0;               // (0 times anything finite, at every vertex)
        }
    }
}
// The blend leaves every value the draw writes as it was when its source term is exactly 0 and its destination factor exactly 1.
bool PixBlendNoop(const RenderState& rs, const FsValueOut& o) {
    if (rs.depthTest && rs.depthWrite) return false;
    if (!rs.colorWrite && !rs.alphaWrite) return false; // (a no-op by its state alone: GXV_PX_NOOP's)
    if (!rs.blend || (rs.blendEq != BE_ADD && rs.blendEq != BE_REVSUB)) return false;
    const uint8_t sf = rs.blendSrc, df = rs.blendDst;
    if (rs.colorWrite) {
        const bool srcZero = o.colorZero || sf == BF_ZERO || (sf == BF_SRC_ALPHA && o.alphaZero);
        const bool dstOne = df == BF_ONE || (df == BF_INV_SRC_ALPHA && o.alphaZero) || (df == BF_INV_SRC_COLOR && o.colorZero);
        if (!srcZero || !dstOne) return false;
    }
    if (rs.alphaWrite) {
        if (rs.dstAlphaZero) return false; // (separate alpha factors: ZERO / ZERO or ONE / ZERO write the alpha)
        // (glBlendFunc: on the alpha channel SRC_COLOR is the source alpha, ONE_MINUS_SRC_COLOR one minus it)
        const bool srcZero = o.alphaZero || sf == BF_ZERO;
        const bool dstOne = df == BF_ONE || ((df == BF_INV_SRC_ALPHA || df == BF_INV_SRC_COLOR) && o.alphaZero);
        if (!srcZero || !dstOne) return false;
    }
    return true;
}
// gpu960: whether the draw's blend state is one an alpha of 1 turns into an overwrite (the factors, and that the alpha channel is not blended differently).
bool PixBlendCandidate(const RenderState& rs) {
    if (!rs.blend || rs.blendEq != BE_ADD) return false;
    if (!rs.colorWrite && !rs.alphaWrite) return false;
    if (rs.alphaWrite && rs.dstAlphaZero) return false; // (the alpha channel has factors of its own there: not the same overwrite)
    const bool srcOk = rs.blendSrc == BF_ONE || rs.blendSrc == BF_SRC_ALPHA;
    const bool dstOk = rs.blendDst == BF_ZERO || rs.blendDst == BF_INV_SRC_ALPHA;
    return srcOk && dstOk;
}
// 0 not an overwrite; 1 an overwrite whatever the destination (exact: the factors are 1 and 0 exactly); 2 within `levels` byte levels (alpha proven >= 1 - levels / 255).
int PixBlendOverwrite(const RenderState& rs, const FsValueOut& o, double levels) {
    if (!PixBlendCandidate(rs)) return 0;
    const bool needsAlpha = rs.blendSrc == BF_SRC_ALPHA || rs.blendDst == BF_INV_SRC_ALPHA;
    if (!needsAlpha) return 1;                                  // (ONE / ZERO: the blend was an overwrite already)
    if (o.alphaOne) return 1;                                   // (SRC_ALPHA x 1.0 + dst x 0.0: both exact in any blender)
    // (near: the colour differs from the source by (1 - alpha) x |dst - src|, the alpha channel by twice that when it is written with SRC_ALPHA)
    const double lost = (1.0 - static_cast<double>(o.alphaLoOut)) * 255.0 * ((rs.alphaWrite && rs.blendSrc == BF_SRC_ALPHA) ? 2.0 : 1.0);
    return lost <= levels ? 2 : 0;
}
void PixGather(const FsKey& k, FsValueIn& in);
// FsValueEval behind a memo of its inputs (a material's draws come round with the same values picture after picture): 512 entries,
// direct-mapped by the hash of the fragment shader id and every input byte; a hit compares the hash only (64 bits).
struct PixMemo { uint64_t h = 0; FsValueOut o; bool valid = false; };
PixMemo g_pixMemo[512];
void PixEval(uint16_t id, const FsKey& k, const FsValueIn& in, FsValueOut& o) {
    const uint64_t h = XXH3_64bits_withSeed(&in, sizeof in, 0x9E3779B97F4A7C15ull ^ id);
    PixMemo& m = g_pixMemo[(h ^ (h >> 29)) & 511u];
    if (m.valid && m.h == h) { o = m.o; ++g_pix.memoHits; return; }
    FsValueEval(k, in, o);
    ++g_pix.evals;
    m.h = h; m.o = o; m.valid = true;
}
// The rasterised colour channels the key reads whose value comes from the vertices' own colours (no channel: the attribute itself; a
// material from the vertex colour), bit ch: a draw's vertex colours can then prove more than the resolution could (PixVtxRecheck).
uint8_t PixVtxChannels(const FsKey& k, const VsKey& vs) {
    uint8_t read = 0, m = 0;
    for (int s = 0; s < k.numStages; ++s)
        if (k.stage[s].rasChan < 2) read |= static_cast<uint8_t>(1u << k.stage[s].rasChan);
    for (int ch = 0; ch < 2; ++ch) {
        if (!(read & (1u << ch))) continue;
        if (ch >= vs.numChans || vs.chan[ch].matSrcVtx || vs.chan[ch + 2].matSrcVtx) m |= static_cast<uint8_t>(1u << ch);
    }
    return PixLitDebug() ? 0 : m;
}
// Decide for the draw being resolved (fragment shader `id`, before the rejectZero / alpha-dead variants); allowAt: may take the atPass
// variant. Returns the id to draw with (before RejectZeroFs).
uint16_t PixDecide(uint16_t id, const RenderState& rs, bool allowAt) {
    cur.pixZero = 0;
    cur.pixVtx = 0;
    cur.pixAt = false;
    cur.pixTest = false;
    cur.pixBase = id;
    for (int c = 0; c < 4; ++c) {
        cur.pixRegs[c] = static_cast<int16_t>(g.tevReg[1][c]);
        cur.pixRegs[4 + c] = static_cast<int16_t>(g.tevReg[2][c]);
    }
    if (id >= g_fsReg.keys.size()) return id;
    const FsKey& k = g_fsReg.keys[id];
    const bool tested = FsHasAlphaTestText(k);
    cur.pixTest = tested;
    const bool zeroCand = PixZeroMode() != 0 && (tested || (rs.blend && !(rs.depthTest && rs.depthWrite) && (rs.colorWrite || rs.alphaWrite) &&
                                                             (rs.blendDst == BF_ONE || rs.blendDst == BF_INV_SRC_ALPHA || rs.blendDst == BF_INV_SRC_COLOR)));
    const bool atCand = allowAt && PixAtPassMode() != 0 && tested && !k.rejectZero() && !k.regFromVtx && !cur.fold;
    // gpu960: a blended draw whose blend can be an overwrite (see PixBlendOverwrite), and the census's own candidates (every tested / blended draw)
    cur.pixBlendWant = 0;
    cur.pixCls = static_cast<uint8_t>((tested ? PIXC_TEST : 0) | (rs.blend ? PIXC_BLEND : 0) | (!rs.blend && !tested ? PIXC_OPAQUE : 0));
    cur.pixFs = id;
    cur.pixAlphaLo = 0.0f;
    cur.pixAlphaHi = 1.0f;
    const bool boCand = PixBlendOffMode() != 0 && rs.blend && !cur.fold && !k.regFromVtx && PixBlendCandidate(rs);
    const bool cenCand = allowAt && PixCensusOn() && (tested || rs.blend);
    if (!zeroCand && !atCand && !boCand && !cenCand) return id;
    if (PixZeroMode() == 3 && !atCand && !boCand && !cenCand) {
        // (defaults48, the lean way: no evaluation here; a blended, untested draw whose shader reads a colour that comes from its vertices is marked for PixVtxRecheck)
        if (!tested && cur.vsId < g_vsReg.keys.size()) cur.pixVtx = PixVtxChannels(k, g_vsReg.keys[cur.vsId]);
        return id;
    }
    FsValueIn in;
    std::memset(static_cast<void*>(&in), 0, sizeof in); // (padding too: the memo hashes the bytes)
    PixGather(k, in);
    cur.pixTexK = 0; cur.pixTexN = 0;
    for (int s = 0; s < k.numStages; ++s) { // (heavyopt25: which of the draw's texture maps had a channel range)
        const FsKey::Stage& st = k.stage[s];
        if (!st.texEnable) continue;
        const int m = st.texMap & 7;
        if (cur.pixTexN & (1u << m)) continue;
        cur.pixTexN = static_cast<uint8_t>(cur.pixTexN | (1u << m));
        if (in.texKnown[m]) cur.pixTexK = static_cast<uint8_t>(cur.pixTexK | (1u << m));
    }
    FsValueOut o;
    PixEval(id, k, in, o);
    if (!o.ok) { ++g_pix.notOk; return id; }
    cur.pixAlphaLo = o.alphaLoOut;
    cur.pixAlphaHi = o.alphaHi;
    if (tested) cur.pixCls = static_cast<uint8_t>(cur.pixCls | (o.test == 1 ? PIXC_TPASS : o.test == 0 ? PIXC_TFAIL : (o.testRaw == 1 ? PIXC_TRAW : 0)));
    if (boCand || cenCand) {
        const int ow = PixBlendOverwrite(rs, o, PixBlendOffLevels());
        if (ow == 1) cur.pixCls = static_cast<uint8_t>(cur.pixCls | PIXC_BONE);
        else if (ow == 2) cur.pixCls = static_cast<uint8_t>(cur.pixCls | PIXC_BNEAR);
        if (boCand && (ow == 1 || (ow == 2 && PixBlendOffMode() == 2))) cur.pixBlendWant = static_cast<uint8_t>(ow);
    }
    if (zeroCand) {
        if (tested && o.test == 0) cur.pixZero = 1;
        else if (PixBlendNoop(rs, o)) cur.pixZero = 2;
        else if (!tested && cur.vsId < g_vsReg.keys.size()) cur.pixVtx = PixVtxChannels(k, g_vsReg.keys[cur.vsId]);
    }
    if (atCand && o.test == 1 && !cur.pixZero) {
        if (PixAtPassMode() == 2) { cur.pixAt = true; return id; } // (counting only: no variant made, nothing compiled)
        const uint16_t vid = AtPassFs(id);
        if (vid != id) {
            cur.pixAt = true;
            return vid;
        }
    }
    return id;
}
void PixGather(const FsKey& k, FsValueIn& in) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) { in.reg[r][c] = g.tevReg[r][c]; in.konst[r][c] = g.tevKonst[r][c]; }
    for (int s = 0; s < k.numStages; ++s) {
        const FsKey::Stage& st = k.stage[s];
        if (!st.texEnable) continue;
        const int m = st.texMap & 7;
        if (in.texKnown[m]) continue;
        in.texKnown[m] = cur.tex[m].texId != 0 && TexChannelRange(cur.tex[m].texId, in.texLo[m], in.texHi[m]);
    }
    if (cur.vsId < g_vsReg.keys.size()) PixRas(g_vsReg.keys[cur.vsId], in);
    in.ref0 = static_cast<int>(bits(g.bp[0xF3], 0, 8));
    in.ref1 = static_cast<int>(bits(g.bp[0xF3], 8, 8));
    in.dstAlpha = static_cast<int>(bits(g.bp[0x42], 0, 8));
    in.fogColor[0] = static_cast<uint8_t>(bits(g.bp[0xF2], 16, 8));
    in.fogColor[1] = static_cast<uint8_t>(bits(g.bp[0xF2], 8, 8));
    in.fogColor[2] = static_cast<uint8_t>(bits(g.bp[0xF2], 0, 8));
}
} // namespace
// gpu960: GXV_PIX_BLENDOFF's two moves on cur.rs. Restore: a draw turned unblended by the last decision gets its blend back (the decision is made again from
// the pristine state); Apply: the proven blend is switched off, with the factors of a plain opaque state (so that the draw merges with, and compiles like,
// the unblended draws of the same shader).
inline void PixBlendRestore(RenderState& rs) {
    if (!cur.pixBlendOff) return;
    rs.blend = cur.pixSavedBlend[0];
    rs.blendSrc = cur.pixSavedBlend[1];
    rs.blendDst = cur.pixSavedBlend[2];
    rs.blendEq = cur.pixSavedBlend[3];
    cur.pixBlendOff = 0;
    ++cur.stateSerial;
}
inline void PixBlendApply(RenderState& rs) {
    if (!cur.pixBlendWant) return;
    cur.pixSavedBlend[0] = rs.blend;
    cur.pixSavedBlend[1] = rs.blendSrc;
    cur.pixSavedBlend[2] = rs.blendDst;
    cur.pixSavedBlend[3] = rs.blendEq;
    rs.blend = 0;
    rs.blendSrc = BF_ONE;
    rs.blendDst = BF_ZERO;
    rs.blendEq = BE_ADD;
    cur.pixBlendOff = cur.pixBlendWant;
    ++cur.stateSerial;
}
std::vector<uint16_t> g_bisectVar; // GXV_GPU_BISECT: per fragment shader id, its substitute's id + 1 (0 not made, 0xFFFF none)
uint16_t PixBisectFs(uint16_t id, const RenderState& rs, long mode) {
    if (id >= g_fsReg.keys.size() || rs.blend || !rs.depthWrite || !rs.depthTest || FsHasAlphaTestText(g_fsReg.keys[id])) return id;
    if (g_bisectVar.size() <= id) g_bisectVar.resize(static_cast<size_t>(id) + 1, 0);
    if (g_bisectVar[id]) return g_bisectVar[id] == 0xFFFFu ? id : static_cast<uint16_t>(g_bisectVar[id] - 1);
    FsKey v = g_fsReg.keys[id]; // (a copy: Lookup may grow the registry)
    bool ok = v.numIndStages == 0 && !v.regFromVtx;
    bool texd = false, readsRas = false;
    for (int s = 0; s < v.numStages; ++s) { texd = texd || v.stage[s].texEnable; readsRas = readsRas || v.stage[s].rasChan == 0; }
    ok = ok && texd && (mode == 2 || readsRas);
    uint16_t out = 0xFFFFu;
    if (ok) {
        if (mode == 1) {
            // (one stage: the rasterised colour of channel 0, alpha too; no texture, no fog, no indirect, no test)
            const uint8_t tg = v.numTexGens;
            v = FsKey{};
            v.numTexGens = tg;
            v.numStages = 1;
            v.stage[0].color = (0xFu << 12) | (0xFu << 8) | (0xFu << 4) | 0xAu | (1u << 19);
            v.stage[0].alpha = (7u << 13) | (7u << 10) | (7u << 7) | (5u << 4) | (1u << 19);
            v.stage[0].rasChan = 0;
        } else {
            for (int s = 0; s < v.numStages; ++s)
                if (v.stage[s].texEnable) { v.stage[s].texEnable = 0; v.stage[s].texOff = 1; v.stage[s].lodBias = 0; }
        }
        const uint16_t vid = Lookup(g_fsReg, v, true, GenerateFragmentShader);
        if (vid != id) out = vid;
    }
    g_bisectVar[id] = out == 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(out + 1);
    return out == 0xFFFFu ? id : out;
}
// ---- GXV_DIAG_FETCHONLY=<hex key>[,<hex key>...] (gpu27, DIAGNOSTIC, NOT exact, default off): a draw whose fragment shader's key hash (the "fh" of the scene dump, the low 32 bits of
// ShaderSource::keyHash, e.g. f9d97a8b = Freezeflame's additive glow sheet) is listed is drawn with a fetch-only shader: ONE stage that outputs the texture of the key's first
// texture stage (same map, same texgen, same swizzle) as colour and alpha, nothing else (no TEV arithmetic, no indirect warp, no fog, no konst, no vertex colour). The render state
// stays as it is (blend, depth, cull, scissor, viewport) and so does the geometry, so the covered pixels, the blend reads and the texture reads are the draw's own; only the
// per-pixel arithmetic is gone. Read the console run against the control: gpu-scenes 3D ms of the scene that holds the draw. A saving of most of the key's priced ms (Freezeflame:
// about 5.9 ms by deletion) means arithmetic-bound (then trimming the key's TEV text / half precision pays); a saving near zero means fill / blend / texture-bound (then only fewer or
// smaller pixels pay: the coverage cut, the compositor). The picture is visibly wrong while it runs (colours of the sheet are the raw texture): a measurement, never a default.
// Log (basic log, every 10 s while on): "diag-fetchonly: keys ..: N draws a picture (K indices), V variants (R refused: no texture stage)".
namespace {
struct DiagFetch {
    bool read = false;
    std::vector<uint32_t> keys;
    std::vector<uint16_t> var; // per fragment shader id: its variant's id + 1 (0 not decided, 0xFFFF not matched / refused)
    uint64_t draws = 0, idx = 0, made = 0, refused = 0, lastFrame = 0;
    FeClock::Time last = FeClock::Read();
};
DiagFetch g_dfo;
inline bool DiagFetchOn() {
    DiagFetch& d = g_dfo;
    if (!d.read) {
        d.read = true;
        for (const char* q = diag::Env("GXV_DIAG_FETCHONLY"); q && *q;) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(q, &end, 16);
            if (end == q) break;
            d.keys.push_back(static_cast<uint32_t>(v));
            q = *end == ',' ? end + 1 : end;
        }
    }
    return !d.keys.empty();
}
uint16_t DiagFetchFs(uint16_t id) {
    DiagFetch& d = g_dfo;
    if (id >= g_fsReg.keys.size()) return id;
    if (d.var.size() <= id) d.var.resize(static_cast<size_t>(id) + 1, 0);
    if (!d.var[id]) {
        uint16_t out = 0xFFFFu;
        const bool have = id < g_fsReg.sources.size() && g_fsReg.sources[id];
        if (have && std::find(d.keys.begin(), d.keys.end(), static_cast<uint32_t>(g_fsReg.sources[id]->keyHash)) != d.keys.end()) {
            const FsKey v = g_fsReg.keys[id]; // (a copy: Lookup may grow the registry)
            int t = -1;
            for (int s = 0; s < v.numStages && t < 0; ++s)
                if (v.stage[s].texEnable) t = s;
            if (t < 0) {
                ++d.refused;
            } else {
                FsKey n{};
                n.numTexGens = v.numTexGens;
                n.texProjMask = v.texProjMask;
                n.numStages = 1;
                n.alphaTestOn = v.alphaTestOn; // (a key with a test keeps it: the coverage stays the key's, on the texture's alpha)
                n.alphaTest = v.alphaTest;
                n.dstAlpha = v.dstAlpha;
                n.stage[0].texEnable = 1;
                n.stage[0].texMap = v.stage[t].texMap;
                n.stage[0].texCoord = v.stage[t].texCoord;
                n.stage[0].texSwap = v.stage[t].texSwap;
                n.stage[0].rasChan = 7;
                n.stage[0].color = (0xFu << 12) | (0xFu << 8) | (0xFu << 4) | 0x8u | (1u << 19); // D = TEXC, clamped
                n.stage[0].alpha = (7u << 13) | (7u << 10) | (7u << 7) | (4u << 4) | (1u << 19); // D = TEXA, clamped
                n.setTexVec2(v.texVec2());
                const uint16_t vid = Lookup(g_fsReg, n, true, GenerateFragmentShader);
                if (vid != id) { out = vid; ++d.made; }
            }
        }
        d.var[id] = out == 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(out + 1);
    }
    return d.var[id] == 0xFFFFu ? id : static_cast<uint16_t>(d.var[id] - 1);
}
void DiagFetchNote(uint32_t idxCount) { // (an emitted draw that took the variant)
    ++g_dfo.draws;
    g_dfo.idx += idxCount;
}
void DiagFetchLog() {
    DiagFetch& d = g_dfo;
    if (!d.read || d.keys.empty()) return;
    const auto now = FeClock::Picture();
    if (now - d.last < std::chrono::seconds(10)) return;
    const double pics = static_cast<double>(g_frame > d.lastFrame ? g_frame - d.lastFrame : 1);
    d.last = now;
    d.lastFrame = g_frame;
    char ks[120];
    size_t o = 0;
    for (size_t i = 0; i < d.keys.size() && o + 12 < sizeof ks; ++i) o += static_cast<size_t>(std::snprintf(ks + o, sizeof ks - o, "%s%08x", i ? "," : "", static_cast<unsigned>(d.keys[i])));
    if (port_log_summary)
        port_log_summary("diag-fetchonly: GXV_DIAG_FETCHONLY=%s (DIAGNOSTIC, the picture is wrong while it runs): %.1f draws a picture drawn fetch-only (%.1fk indices), %u variants made, %u refused (no texture stage)",
                         ks, static_cast<double>(d.draws) / pics, static_cast<double>(d.idx) / pics / 1000.0, static_cast<unsigned>(d.made), static_cast<unsigned>(d.refused));
    d.draws = d.idx = 0;
}
} // namespace
uint16_t PixUseFs(uint16_t id, RenderState& rs) {
    PixBlendRestore(rs);
    cur.pixBlendWant = 0;
    if (PixAny()) id = PixDecide(id, rs, true);
    PixBlendApply(rs);
    const long bis = PixBisectMode();
    if (bis != 0) {
        const uint16_t b = PixBisectFs(id, rs, bis);
        cur.pixBis = b != id ? 1 : 0;
        id = b;
    } else {
        cur.pixBis = 0;
    }
    if (DiagFetchOn()) { // (GXV_DIAG_FETCHONLY, gpu27: a diagnostic substitute for the listed keys)
        const uint16_t f = DiagFetchFs(id);
        cur.diagFetch = f != id ? 1 : 0;
        id = f;
    } else {
        cur.diagFetch = 0;
    }
    return PrepassFs(RejectZeroFs(id, rs), rs); // (GXV_ALPHATEST_PREPASS: with the switch off: id unchanged)
}
// A folded draw (GXV_FOLDREGS) carries TEV registers 1 and 2 in its vertices: the resolution's fast path does not run again when only
// they change, so the GXV_PIX_ZERO decision is made again here when they differ from the ones it was made with.
inline void PixFoldRecheck() {
    bool same = true;
    for (int c = 0; c < 4; ++c)
        if (cur.pixRegs[c] != g.tevReg[1][c] || cur.pixRegs[4 + c] != g.tevReg[2][c]) same = false;
    if (same) return;
    const bool at = cur.pixAt;
    (void)PixDecide(cur.pixBase, cur.rs, false);
    cur.pixAt = at;
}
// A small dynamic draw whose blend was not proven a no-op by the resolution, and whose fragment shader reads colours that come from its
// vertices (a layout pane's alpha, a particle's colour): its vertices' colours (decoded, 4 x u8 a colour) give the ranges instead of
// 0..1. Only when one of those components is 0 at every vertex (nothing else can make an exact 0 that the resolution did not see). For
// this draw only (the next draw with the same state has other vertices): returns 2 when the blend is a no-op, else 0. (vtx: the
// draw's decoded vertices, already in place.)
uint8_t PixVtxRecheck(const uint8_t* vtx, uint32_t count, const VtxLayout& L) {
    uint8_t mx[2][4] = {}, mn[2][4];
    std::memset(mn, 255, sizeof mn);
    bool anyZero = false;
    for (int ch = 0; ch < 2; ++ch) {
        if (!(cur.pixVtx & (1u << ch))) continue;
        if (!(L.mask & (VL_COL0 << ch))) return 0; // (the attribute is a constant the back end sets: not known here)
        const uint8_t off = ch ? L.offCol1 : L.offCol0;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* c = vtx + static_cast<size_t>(i) * L.stride + off;
            for (int k = 0; k < 4; ++k) { mx[ch][k] = std::max(mx[ch][k], c[k]); mn[ch][k] = std::min(mn[ch][k], c[k]); }
        }
        for (int k = 0; k < 4; ++k) anyZero = anyZero || mx[ch][k] == 0;
    }
    if (!anyZero || cur.pixBase >= g_fsReg.keys.size() || cur.vsId >= g_vsReg.keys.size()) return 0;
    const FsKey& k = g_fsReg.keys[cur.pixBase];
    const VsKey& vs = g_vsReg.keys[cur.vsId];
    FsValueIn in;
    std::memset(static_cast<void*>(&in), 0, sizeof in);
    PixGather(k, in);
    for (int ch = 0; ch < 2; ++ch) {
        if (!(cur.pixVtx & (1u << ch))) continue;
        for (int comp = 0; comp < 4; ++comp) {
            const bool fromVtx = ch >= vs.numChans || vs.chan[ch + (comp == 3 ? 2 : 0)].matSrcVtx;
            if (!fromVtx) continue; // (the material register: as PixRas made it)
            const bool lit = ch < vs.numChans && vs.chan[ch + (comp == 3 ? 2 : 0)].enable;
            in.rasHi[ch][comp] = mx[ch][comp] / 255.0f;
            in.rasLo[ch][comp] = lit ? 0.0f : mn[ch][comp] / 255.0f; // (lit: times a light sum clamped to 0..1)
            in.rasZero[ch][comp] = mx[ch][comp] == 0;
        }
    }
    FsValueOut o;
    PixEval(cur.pixBase, k, in, o);
    ++g_pix.vtxChecks;
    return o.ok && PixBlendNoop(cur.rs, o) ? 2 : 0;
}
// At the emission of a draw (after the culls): counts, and true when GXV_PIX_ZERO=1 leaves it out. zero: cur.pixZero, or what the
// draw's own vertex colours proved (PixVtxRecheck).
inline bool PixDrop(uint32_t idxCount, uint8_t zero) {
    if (cur.diagFetch) DiagFetchNote(idxCount); // (GXV_DIAG_FETCHONLY census)
    if (!PixAny()) return false;
    PixCtr& c = g_pix;
    ++c.draws;
    if (cur.rs.blend) ++c.blended;
    if (cur.pixTest) ++c.tested;
    if (cur.pixAt) { ++c.atDraws; c.atIdx += idxCount; }
    if (PixCensusOn() || cur.pixBlendOff || cur.pixBis) {
        const bool unk = g_pixFrac < 0.0f;
        const double f = unk ? 1.0 : static_cast<double>(g_pixFrac);
        g_pixFrac = -1.0f;
        if (PixCensusOn()) {
            if (unk) ++c.unmeasured;
            auto add = [&](PixCtr::Row& r) { ++r.n; r.idx += idxCount; r.scr += f; };
            const uint8_t m = cur.pixCls;
            if (m & PIXC_TEST) add(c.cTest);
            if (m & PIXC_TPASS) add(c.cTpass);
            if (m & PIXC_TRAW) add(c.cTraw);
            if (m & PIXC_TFAIL) add(c.cTfail);
            if (m & PIXC_BLEND) add(c.cBlend);
            if (m & PIXC_BONE) add(c.cBone);
            if (m & PIXC_BNEAR) add(c.cBnear);
            if (m & PIXC_OPAQUE) add(c.cOpaque);
            if (PixCensus2() && (m & (PIXC_TEST | PIXC_BLEND))) {
                PixRow* t = nullptr;
                const uint8_t key = static_cast<uint8_t>(m & (PIXC_TEST | PIXC_BLEND));
                for (PixRow& x : g_pixRows)
                    if (x.fs == cur.pixFs && x.vs == cur.vsId && x.cls == key && x.src == cur.rs.blendSrc && x.dst == cur.rs.blendDst && x.dw == (cur.rs.depthWrite ? 1 : 0)) t = &x;
                if (!t && g_pixRows.size() < 96) {
                    g_pixRows.push_back(PixRow{cur.pixFs, static_cast<uint16_t>(cur.vsId), key, static_cast<uint8_t>(cur.rs.blendSrc), static_cast<uint8_t>(cur.rs.blendDst),
                                               static_cast<uint8_t>(cur.rs.depthWrite ? 1 : 0), cur.pixTexK, cur.pixTexN, 0, 0, 0.0, 1.0f, 0.0f});
                    t = &g_pixRows.back();
                }
                if (t) { ++t->n; t->idx += idxCount; t->scr += f; t->aLo = std::min(t->aLo, cur.pixAlphaLo); t->aHi = std::max(t->aHi, cur.pixAlphaHi); }
            }
            if ((m & (PIXC_TRAW | PIXC_BNEAR)) && !cur.pixAt) {
                PixTop* t = nullptr;
                for (PixTop& x : g_pixTop) if (x.fs == cur.pixFs && x.cls == (m & (PIXC_TRAW | PIXC_BNEAR))) t = &x;
                if (!t && g_pixTop.size() < 24) { g_pixTop.push_back(PixTop{cur.pixFs, static_cast<uint8_t>(m & (PIXC_TRAW | PIXC_BNEAR)), 0, 0.0, cur.pixAlphaLo, cur.pixAlphaHi}); t = &g_pixTop.back(); }
                if (t) { ++t->n; t->scr += f; t->aLo = std::min(t->aLo, cur.pixAlphaLo); t->aHi = std::max(t->aHi, cur.pixAlphaHi); }
            }
        }
        if (cur.pixBlendOff) { if (cur.pixBlendOff == 1) ++c.blendOff; else ++c.blendOffNear; c.blendOffScr += f; }
        if (cur.pixBis) { ++c.bisect; c.bisectIdx += idxCount; }
    } else {
        g_pixFrac = -1.0f;
    }
    if (!zero) return false;
    if (zero == 1) ++c.zeroKill;
    else ++c.zeroBlend;
    if (zero == 2 && !cur.pixZero) ++c.zeroVtx;
    c.zeroIdx += idxCount;
    // (defaults48) Never in a picture that opens a 60 Assist pair (GXV_HRW): the pair's second picture is built from this picture's HUD draws and their rectangles, and
    // with the layout's alpha-0 panes left out the HUD flickered and looked faded (the owner on the Vita, 22 Nov, build c36e, 60 Assist forced on: wrong with
    // GXV_PIX_ZERO=3, right with 0, nothing else changed). The draw is then made as without the switch; counted in the "pix23:" line.
    if (g_buf.hrwId != 0) { ++c.zeroPairKept; return false; }
    return PixZeroMode() == 1 || PixZeroMode() == 3;
}
inline bool PixDrop(uint32_t idxCount) { return PixDrop(idxCount, cur.pixZero); }
// fs24: the "fogfs24:" line every 10 s while GXV_FS_HALF_FOG or GXV_FS_NOCOLOR is on (totals since the start; the front end's draws, not pictures).
void FogFs24Log() {
    if (!FsHalfFogMode() && !FsNoColorMode()) return;
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::seconds(10)) return;
    last = now;
    unsigned nHalfFog = 0, nFogKeys = 0;
    for (const FsKey& k : g_fsReg.keys) { // (the fragment shader keys made so far: fog keys, and of them the ones with half-exact lanes)
        if (k.fog & kFogVtxBit) ++nFogKeys;
        if (k.fog & kFogHalfBit) ++nHalfFog;
    }
    char line[440];
    std::snprintf(line, sizeof line,
                  "fogfs24: GXV_FS_HALF_FOG=%d: %llu draws with half-exact fog lanes (%llu with |bs| > 4096: must be 0), %u of %u vertex-fog fragment keys marked for half; "
                  "GXV_FS_NOCOLOR=%d: %llu draws with the colorDead variant, %llu variants made",
                  FsHalfFogMode() ? 1 : 0, static_cast<unsigned long long>(g_fogHalfDraws.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(g_fogHalfWide.load(std::memory_order_relaxed)), nHalfFog, nFogKeys, FsNoColorMode() ? 1 : 0,
                  static_cast<unsigned long long>(g_noColorDraws.load(std::memory_order_relaxed)), static_cast<unsigned long long>(g_noColorMade.load(std::memory_order_relaxed)));
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
}
// fsregs25: the "fsregs:" line every 10 s while GXV_FS_REGS is on (totals since the start).
void FsRegsLog() {
    if (!FsRegsMode()) return;
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::seconds(10)) return;
    last = now;
    FsRegsStats st;
    FsRegsCounters(st);
    const double n = st.shaders ? static_cast<double>(st.shaders) : 1.0;
    char line[360];
    std::snprintf(line, sizeof line,
                  "fsregs: GXV_FS_REGS=1: %llu shaders compacted (u_tev mean %.2f -> %.2f vec4, max %llu -> %llu; %llu reg parts elided, %llu uvs sunk, %llu ind sunk, %llu fallbacks)",
                  static_cast<unsigned long long>(st.shaders), st.rowsOld / n, st.rowsNew / n,
                  static_cast<unsigned long long>(st.maxOld), static_cast<unsigned long long>(st.maxNew),
                  static_cast<unsigned long long>(st.regsElided), static_cast<unsigned long long>(st.uvsSunk),
                  static_cast<unsigned long long>(st.indSunk), static_cast<unsigned long long>(st.fallbacks));
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
}
void PixLog() {
    FogFs24Log();
    FsRegsLog();
    DiagFetchLog(); // (GXV_DIAG_FETCHONLY: its own line)
    if (!PixAny()) return;
    ++g_pix.pictures;
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::seconds(10)) return;
    const double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    const PixCtr& k = g_pix;
    const double n = k.pictures ? static_cast<double>(k.pictures) : 1.0;
    const long zm = PixZeroMode(), am = PixAtPassMode();
    char line[1024];
    std::snprintf(line, sizeof line,
                  "pix23: %.0f s, %llu pictures; a picture: %.1f draws (%.1f blended, %.1f alpha-tested); GXV_PIX_ZERO=%ld: %s %.1f draws (%.1f whose "
                  "alpha test fails everywhere, %.1f whose blend leaves the target as it is; %.1fk indices); GXV_PIX_ATPASS=%ld: %.1f of the "
                  "alpha-tested draws %s without the test (%.1fk indices); evaluations %.1f (+%.1f from the memo; %.1f keys not analysed), draws "
                  "re-checked with their vertex colours %.1f (%.1f of them no-ops); drawn all the same in a 60 Assist pair's picture %.1f; since the start: %llu atPass variants made, %llu refused",
                  secs, static_cast<unsigned long long>(k.pictures), k.draws / n, k.blended / n, k.tested / n, zm, zm == 1 || zm == 3 ? "left out" : "would leave out",
                  (k.zeroKill + k.zeroBlend) / n, k.zeroKill / n, k.zeroBlend / n, k.zeroIdx / n / 1000.0, am, k.atDraws / n,
                  am == 1 ? "drawn" : "could be drawn", k.atIdx / n / 1000.0, k.evals / n, k.memoHits / n, k.notOk / n, k.vtxChecks / n, k.zeroVtx / n, k.zeroPairKept / n,
                  static_cast<unsigned long long>(g_pixTotal.atMade), static_cast<unsigned long long>(g_pixTotal.atRefused));
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    if (PixCensusOn() || PixBlendOffMode() != 0 || PixBisectMode() != 0) {
        // gpu960: a picture's numbers; screens are an upper bound (a static run: its list's box projected; a small dynamic draw: its bounds; else 1.00)
        auto R = [&](const PixCtr::Row& r) { char b[64]; std::snprintf(b, sizeof b, "%.1f dr %.1fk idx %.2f scr", r.n / n, r.idx / n / 1000.0, r.scr / n); return std::string(b); };
        char l2[1400];
        int w = std::snprintf(l2, sizeof l2,
                  "gpu960: %.0f s, %llu pictures; GXV_PIX_CENSUS=%d GXV_PIX_BLENDOFF=%ld (levels %.1f) GXV_GPU_BISECT=%ld; a picture: opaque (no blend, no test) %s; "
                  "alpha-tested %s: provably always pass %s, pass without the filter margin %s, always fail %s; blended %s: alpha exactly 1 under an overwrite blend %s, "
                  "within the levels %s; drawn unblended: %.1f exact + %.1f within the levels (%.2f scr); bisect substitutes %.1f dr (%.1fk idx); no bound %.1f dr",
                  secs, static_cast<unsigned long long>(k.pictures), PixCensusOn() ? 1 : 0, PixBlendOffMode(), PixBlendOffLevels(), PixBisectMode(), R(k.cOpaque).c_str(),
                  R(k.cTest).c_str(), R(k.cTpass).c_str(), R(k.cTraw).c_str(), R(k.cTfail).c_str(), R(k.cBlend).c_str(), R(k.cBone).c_str(), R(k.cBnear).c_str(),
                  k.blendOff / n, k.blendOffNear / n, k.blendOffScr / n, k.bisect / n, k.bisectIdx / n / 1000.0, k.unmeasured / n);
        std::sort(g_pixTop.begin(), g_pixTop.end(), [](const PixTop& a, const PixTop& b) { return a.scr > b.scr; });
        for (size_t i = 0; i < g_pixTop.size() && i < 4 && w > 0 && w < static_cast<int>(sizeof l2) - 160; ++i) {
            const PixTop& x = g_pixTop[i];
            w += std::snprintf(l2 + w, sizeof l2 - static_cast<size_t>(w), "%s fs %u (%s) %.1f dr %.2f scr alpha %.3f..%.3f", i ? ";" : "; near misses:", x.fs,
                               (x.cls & PIXC_TRAW) ? "test passes without the margin" : "blend within a few levels", x.n / n, x.scr / n, x.aLo, x.aHi);
        }
        g_pixTop.clear();
        if (port_log_summary) port_log_summary("%s", l2);
        else std::fprintf(stderr, "[vgx] %s\n", l2);
        if (PixCensus2()) { // (heavyopt25: the 14 rows with the most covered screens, per picture; scr is an upper bound for draws without a bound ("unmeasured" above))
            std::sort(g_pixRows.begin(), g_pixRows.end(), [](const PixRow& a, const PixRow& b) { return a.scr > b.scr; });
            for (size_t i = 0; i < g_pixRows.size() && i < 14; ++i) {
                const PixRow& x = g_pixRows[i];
                char l3[360];
                std::snprintf(l3, sizeof l3, "gpu960 row: fs %u vs %u %s%s blend src %u dst %u zwrite %u: %.2f dr %.2fk idx %.2f scr a picture; alpha %.3f..%.3f; texture maps read %02x with a channel range %02x",
                              x.fs, x.vs, (x.cls & PIXC_TEST) ? "alpha-tested" : "", (x.cls & PIXC_BLEND) ? " blended" : "", x.src, x.dst, x.dw, x.n / n, x.idx / n / 1000.0, x.scr / n,
                              x.aLo, x.aHi, x.texN, x.texK);
                if (port_log_summary) port_log_summary("%s", l3);
            }
            g_pixRows.clear();
        }
    }
    PixCtr& t = g_pixTotal;
    t.pictures += k.pictures; t.draws += k.draws; t.blended += k.blended; t.tested += k.tested; t.zeroKill += k.zeroKill;
    t.zeroBlend += k.zeroBlend; t.zeroIdx += k.zeroIdx; t.atDraws += k.atDraws; t.atIdx += k.atIdx; t.evals += k.evals; t.notOk += k.notOk;
    t.vtxChecks += k.vtxChecks; t.zeroVtx += k.zeroVtx; t.memoHits += k.memoHits;
    g_pix = PixCtr{};
}
inline bool FoldValuesOk() {
    uint32_t any = 0;
    for (int c = 0; c < 4; ++c) any |= static_cast<uint32_t>(g.tevReg[1][c]) | static_cast<uint32_t>(g.tevReg[2][c]);
    return (any & ~0xFFu) == 0; // 0..255 each (the registers are signed 11-bit)
}

// The uniform blocks of the current state: built here, or (front-end split, vgx_fesplit.h) sent to the finishing thread.
inline split::BlockCtx MainBlockCtx() {
    if (__builtin_expect(g_warmScope, 0)) return split::BlockCtx{&g_warmUni, g_warmBlockOff, g_warmBlockLen, g_blocksStale, false, g_frame}; // (prewarm24)
    return split::BlockCtx{&g_buf.uni, g_blockOff, g_blockLen, g_blocksStale, true, g_frame};
}
// GXV_J3DNATIVE=5 (vgx_j3native2_draw.inc): whether what the templated runs sent last of these blocks is still the finishing thread's
// current block; every send below clears its flag (the templated path sets it again after its own sends).
struct J3n2MirFlags { bool proj = false, light = false, tev = false; } g_j3n2Mir;
inline void BlkProj() {
    g_j3n2Mir.proj = false;
    if (split::g_on && !g_warmScope) { split::Proj(g_blocksStale); return; }
    split::BlockCtx c = MainBlockCtx();
    split::BuildProjC(c, g);
}
inline uint64_t BlkMtx(const VsKey& vs, bool soft) {
    if (split::g_on && !g_warmScope) return split::Mtx(cur.vsId, vs, soft, g_blocksStale);
    split::BlockCtx c = MainBlockCtx();
    return split::BuildMtxC(c, g, vs, soft);
}
inline void BlkLight(const VsKey& vs) {
    g_j3n2Mir.light = false;
    if (split::g_on && !g_warmScope) { split::Light(cur.vsId, vs, g_blocksStale); return; }
    split::BlockCtx c = MainBlockCtx();
    split::BuildLightC(c, g, vs);
}
inline void BlkTev(const FsKey& fs, const uint16_t texW[8], const uint16_t texH[8], bool fold) {
    g_j3n2Mir.tev = false;
    if (split::g_on && !g_warmScope) { split::Tev(cur.fsId, fs, texW, texH, fold, g_blocksStale); return; }
    split::BlockCtx c = MainBlockCtx();
    split::BuildTevC(c, g, fs, texW, texH, fold);
}
// (the builders as they were called before the split: into this frame's arena, from the current registers)
[[maybe_unused]] void BuildLight(const VsKey& vs) { split::BlockCtx c = MainBlockCtx(); split::BuildLightC(c, g, vs); }
[[maybe_unused]] uint64_t BuildMtx(const VsKey& vs, bool soft) { split::BlockCtx c = MainBlockCtx(); return split::BuildMtxC(c, g, vs, soft); }

// Which palette a draw with a per-vertex position matrix can have, from the highest row its vertices name (g_pendingPosMtxMax,
// 0xFF: not known): 0 the full one, 1 the short one (rows 0..29), 2 the small one (rows 0..11, GXV_PALROWS=1). A key built for a
// class is exact for every draw of the class: the shader reads rows pi..pi+2 and the same normal rows, and pi never exceeds the maximum.
// GXV_PALROWS=2 (census) changes nothing and counts the draws by that maximum.
struct PalCensus {
    uint64_t draws = 0, byMax[10] = {}, beyond = 0, unknown = 0;
} g_palCensus;
inline uint8_t PalClass(uint32_t maxRow, bool packing, bool perVtxPos) {
    if (perVtxPos && PaletteRowsMode() == 2) {
        ++g_palCensus.draws;
        if (maxRow == 0xFF) ++g_palCensus.unknown;
        else if (maxRow > static_cast<uint32_t>(kPalShortMaxRow)) ++g_palCensus.beyond;
        else ++g_palCensus.byMax[maxRow / 3];
    }
    if (!packing || maxRow > static_cast<uint32_t>(kPalShortMaxRow)) return 0;
    return (PaletteRowsMode() == 1 && maxRow <= static_cast<uint32_t>(kPalSmallMaxRow)) ? 2 : 1;
}
static void ResolveStateFull(uint32_t vcdMtxMask, uint32_t vertexCount, uint8_t palCls);
// Most draws follow one that left everything the keys and blocks are built from as it was (only a particle's vertices, position
// matrix and colours changed): then there is nothing to rebuild, and the only questions are whether the draw is transformed on the
// CPU and folds the registers as the last one did. GXV_RESOLVEFAST=0 always takes the full path, 2 takes both and compares what they left.
// (GXV_RSCACHE=0: this and ResolveStateFull are the path; otherwise ResolveStateCached below.)
void ResolveStateLegacy(uint32_t vcdMtxMask, uint32_t vertexCount) {
    // The short matrix palette is enough when the draw's vertices stay within its rows.
    const uint8_t palCls = PalClass(g_pendingPosMtxMax, UniformPacking(), (vcdMtxMask & 1u) != 0);
    g_pendingPosMtxMax = 0xFF;
    const int fastMode = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_RESOLVEFAST", 1))));
    if (fastMode != 0 && cur.valid && !g_blocksStale && !g.DirtyAny() && vcdMtxMask == cur.vcdMtxMask &&
        !((vcdMtxMask & 1u) && palCls != cur.palCls) && (g.xfPosRowsDirty & cur.mtxRowsUsed) == 0) {
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
                    ResolveStateFull(vcdMtxMask, vertexCount, palCls);
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
    ResolveStateFull(vcdMtxMask, vertexCount, palCls);
}
static void ResolveStateFull(uint32_t vcdMtxMask, uint32_t vertexCount, uint8_t palCls) {
    // (see MarkBP / MarkXFReg in vgx_state.cpp for what sets which flag)
    const bool keysDirty = !cur.valid || g.bpKeyDirty || g.xfKeyDirty || g.xfRegDirty || vcdMtxMask != cur.vcdMtxMask ||
                           ((vcdMtxMask & 1u) && palCls != cur.palCls);
    const bool texDirty = keysDirty || g.texDirty;
    // (a key that was built again and came out the same leaves the blocks that only depend
    // on the key alone)
    bool fsChanged = false, vsChanged = false;
    const bool vcdChanged = !cur.valid || vcdMtxMask != cur.vcdMtxMask;
    if (keysDirty) {
        FsKey fs;
        BuildFsKey(fs);
        VsKey vs;
        BuildVsKey(vs, fs.numTexGens, palCls, fs.fog);
        PairKeys(fs, vs);
        cur.palCls = palCls;
        fsChanged = !cur.valid || !SameStruct(fs, cur.fs);
        vsChanged = !cur.valid || !SameStruct(vs, cur.vs);
        if (fsChanged) { cur.fs = fs; cur.fsId = Lookup(g_fsReg, fs, true, GenerateFragmentShader); cur.fsIdFoldValid = false; }
        if (vsChanged) { cur.vs = vs; cur.vsId = Lookup(g_vsReg, vs, false, GenerateVertexShader); }
        if (fsChanged || vsChanged) cur.foldOk = FoldRegsEnabled() && FoldKeysOk(cur.fs, cur.vs);
        cur.vcdMtxMask = vcdMtxMask;
    }
    if (keysDirty || g.xfViewDirty || g.bpRsDirty) {
        BuildRenderState(cur.rs);
        cur.pixBlendOff = 0; // (gpu960: a freshly built state has the blend it was built with)
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
        const bool perSlot = VGX_KNOB(EnvInt("GXV_TEXSLOTS", 1) != 0) != 0; // 0: look every used texmap up again (the old behaviour)
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
                if (__builtin_expect(g_enumDry, 0)) { cur.slotBind[m] = TexBind{}; cur.slotW[m] = cur.slotH[m] = 1; } // (precomp26: a dry draw uploads nothing)
                else ResolveTexture(m, cur.slotBind[m], cur.slotW[m], cur.slotH[m]);
                cur.slotResolved |= bit;
            }
            cur.tex[m] = cur.slotBind[m];
            cur.texW[m] = cur.slotW[m];
            cur.texH[m] = cur.slotH[m];
        }
        LodSamplerFill(cur.fs, cur.tex); // (GXV_LODBIAS_SAMPLER)
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
        CanonListFsKey(fk);
        cur.fsIdFold = Lookup(g_fsReg, fk, true, GenerateFragmentShader);
        cur.fsIdFoldValid = true;
    }
    // (folded: registers 0 and 1 are not in the block, so a particle's new colour leaves it alone)
    if (g_blocksStale || g.bpDirty || (g.tevColDirty & (fold ? 0xF9u : 0xFFu)) != 0 || fsChanged || fold != cur.fold || (texDirty && cur.fs.numIndStages))
        BlkTev(cur.fs, cur.texW, cur.texH, fold);
    cur.fold = fold;
    cur.useFs = PixUseFs(fold ? cur.fsIdFold : cur.fsId, cur.rs);
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
//     kept up to date by the register writers) with the vertex-matrix mask and the short-palette flag, in a 4-way set-associative
//     table (2,048 sets, vgx_keymemo.h) that holds the shader ids (the keys themselves are the registry's). A changed key is copied from the
//     registry; an unchanged hash means unchanged keys, whatever the flags say.
//   * Everything else is rebuilt on what it is really built from: the render state on the key registers, the viewport and
//     its own registers; the texture bindings when the fragment key or a binding changed; the blocks on their own flags, not
//     on xfRegDirty (no Build function reads the XF registers that flag stands for).
// The result is the same state, block for block (an untouched input rebuilds an identical block, which PushBlock never
// appends): GXV_RSCACHE=2 runs the old path after the cached one for every draw and compares the resolved state, the block
// offsets and the uniform words appended (logged as "rscache: MISMATCH"). 0: the old path only.
// (The hash is 64 bits; two different key-register contents with the same hash would draw with the wrong shader. The odds
// for the few thousand contents a session sees are about 1e-12; the verify mode would show it.)
//   * (The table is 2,048 sets of GXV_KEYMEMO_WAYS ways (default 4; 1 = direct-mapped). The first version was direct-mapped with 1,024
//     entries and missed 7.5% of its lookups in the Comet Observatory, 450 draw records a frame, from conflicts alone: see
//     vgx_keymemo.h. A miss rebuilds the keys and finds them again in the registry, so the ways only change how often that
//     happens.)
KeyMemoTable g_keyMemo;
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

inline uint32_t KeyMemoWays() {
    const uint32_t ways = static_cast<uint32_t>(VGX_KNOB([] { // read once: 1..4 ways per set (GXV_KEYMEMO_WAYS, default 4)
        const long v = EnvInt("GXV_KEYMEMO_WAYS", 4);
        return static_cast<uint32_t>(v < 1 ? 1 : v > static_cast<long>(KeyMemoTable::kMaxWays) ? KeyMemoTable::kMaxWays : v);
    }()));
    return ways;
}
// The legacy path's notion of "the keys may have changed" (it also decides when it records the short-palette flag, and when
// it rebuilds a render state whose depth comes from a Z texture: the cached path does those as it does).
inline bool LegacyKeysDirty(uint32_t vcdMtxMask, uint8_t palCls) {
    return !cur.valid || g.bpKeyDirty || g.xfKeyDirty || g.xfRegDirty || vcdMtxMask != cur.vcdMtxMask || ((vcdMtxMask & 1u) && palCls != cur.palCls);
}
inline bool ZTexReplace() { return bits(g.bp[0xF5], 2, 2) == 2; }
inline bool SameCurKey(uint32_t vcdMtxMask, uint8_t spo) {
    return g_curKey.valid && cur.valid && g_curKey.h == KeyHashNow() && g_curKey.mask == vcdMtxMask && g_curKey.spo == spo;
}

// The keys for the current registers into cur; fsChanged / vsChanged as ResolveStateFull has them.
void ResolveKeysCached(uint32_t vcdMtxMask, uint8_t palCls, uint8_t spo, bool& fsChanged, bool& vsChanged) {
    const uint64_t h = KeyHashNow();
    const uint32_t ways = KeyMemoWays();
    uint32_t slot = 0;
    uint16_t fsId, vsId;
    if (const KeyMemo* hit = g_keyMemo.Find(h, vcdMtxMask, spo, ways, slot)) {
        const KeyMemo& m = *hit;
        fsId = m.fsId;
        vsId = m.vsId;
        ++g_keyMemoHits;
    } else {
        FsKey fs;
        BuildFsKey(fs);
        VsKey vs;
        BuildVsKey(vs, fs.numTexGens, palCls, fs.fog);
        PairKeys(fs, vs);
        fsId = Lookup(g_fsReg, fs, true, GenerateFragmentShader);
        vsId = Lookup(g_vsReg, vs, false, GenerateVertexShader);
        KeyMemo& m = g_keyMemo.Insert(h, vcdMtxMask, spo, ways, slot);
        m.fsId = fsId; m.vsId = vsId;
        m.valid = 1;
        ++g_keyMemoMisses;
    }
    fsChanged = !cur.valid || fsId != cur.fsId;
    vsChanged = !cur.valid || vsId != cur.vsId;
    if (FeCutHint()) {
        // (GXV_FECUT_HINT, vgx_state.h: the registry's keys the copies below read, 13 and 4 lines the game's turn has pushed out of the
        // caches, hinted all at once instead of fetched one by one as the copy reaches them)
        if (fsChanged) FePrefetchLines(&g_fsReg.keys[fsId], sizeof(FsKey), 16);
        if (vsChanged) FePrefetchLines(&g_vsReg.keys[vsId], sizeof(VsKey), 4);
    }
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
    KeyMemo& m = g_keyMemo.At(g_curKey.slot);
    const bool mine = g_curKey.valid && m.valid && m.h == g_curKey.h && m.vcdMtxMask == g_curKey.mask && m.spo == g_curKey.spo && m.fsId == cur.fsId;
    if (mine && m.fsIdFoldValid) return m.fsIdFold;
    FsKey fk = cur.fs;
    fk.regFromVtx = 3;
    CanonListFsKey(fk);
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
    const bool packing = VGX_KNOB(UniformPacking()) != 0;
    const uint8_t palCls = PalClass(g_pendingPosMtxMax, packing, (vcdMtxMask & 1u) != 0);
    g_pendingPosMtxMax = 0xFF;
    const uint8_t spo = (vcdMtxMask & 1u) ? palCls : 0;
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
        const bool legacyKeysDirty = g.bpKeyDirty || g.xfKeyDirty || g.xfRegDirty || ((vcdMtxMask & 1u) && palCls != cur.palCls);
        if (legacyKeysDirty && ZTexReplace()) goto full;
        {
        const bool soft = vertexCount <= SoftXformLimit() && SoftEligibleCur();
        if (soft == cur.soft) {
            const bool fold = soft && cur.foldOk && FoldValuesOk();
            if (fold == cur.fold && (!fold || cur.fsIdFoldValid) && (g.tevColDirty & (fold ? 0xF9u : 0xFFu)) == 0) {
                if (legacyKeysDirty) cur.palCls = palCls;
                ClearResolveDirty();
                cur.vcdGen = g.vcdGen;
                return;
            }
        }
        }
    }
full:
    // ---- keys
    const bool legacyKeysDirty = LegacyKeysDirty(vcdMtxMask, palCls);
    const bool vcdChanged = !cur.valid || vcdMtxMask != cur.vcdMtxMask;
    bool fsChanged = false, vsChanged = false;
    if (legacyKeysDirty) cur.palCls = palCls;
    const bool keysChanged = !sameKeys;
    if (keysChanged) ResolveKeysCached(vcdMtxMask, palCls, spo, fsChanged, vsChanged);
    // ---- render state: key registers (cull, blend, pixel format, Z texture mode), viewport, scissor / depth / line width
    // (a depth taken from a Z texture is read when the state is built: built exactly when the old path built it)
    if (keysChanged || g.xfViewDirty || g.bpRsDirty || (legacyKeysDirty && ZTexReplace())) {
        BuildRenderStateMemo(cur.rs);
        cur.pixBlendOff = 0; // (gpu960: as above)
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
        const bool perSlot = VGX_KNOB(EnvInt("GXV_TEXSLOTS", 1) != 0) != 0;
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
                if (__builtin_expect(g_enumDry, 0)) { cur.slotBind[m] = TexBind{}; cur.slotW[m] = cur.slotH[m] = 1; } // (precomp26: a dry draw uploads nothing)
                else ResolveTexture(m, cur.slotBind[m], cur.slotW[m], cur.slotH[m]);
                cur.slotResolved |= bit;
            }
            cur.tex[m] = cur.slotBind[m];
            cur.texW[m] = cur.slotW[m];
            cur.texH[m] = cur.slotH[m];
        }
        LodSamplerFill(cur.fs, cur.tex); // (GXV_LODBIAS_SAMPLER)
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
    cur.useFs = PixUseFs(fold ? cur.fsIdFold : cur.fsId, cur.rs);
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
// (tests) since the start: [0] draws, [1] alpha test fails everywhere, [2] blend no-op, [3] drawn with the atPass variant (or would),
// [4] atPass variants made, [5] refused, [6] evaluations, [7] not analysed
void PixCounters(uint64_t out[8]) {
    const PixCtr& a = g_pixTotal;
    const PixCtr& b = g_pix;
    out[0] = a.draws + b.draws; out[1] = a.zeroKill + b.zeroKill; out[2] = a.zeroBlend + b.zeroBlend; out[3] = a.atDraws + b.atDraws;
    out[4] = a.atMade; out[5] = a.atRefused; out[6] = a.evals + b.evals; out[7] = a.notOk + b.notOk;
}
void ResolveCacheCounters(uint64_t out[4]) { out[0] = g_rsChecked; out[1] = g_rsBad; out[2] = g_keyMemoHits; out[3] = g_keyMemoMisses; }
// GXV_FE_PREFETCH (called before each top-level CALL_DL, see "look-ahead" in vgx_state.cpp): what the next draw's resolution will read,
// for the registers as they are now. Once the key registers hash to something new (a material was replayed), the key memo's set for
// that hash (with the vertex-matrix mask of the current descriptor and the palette class of the last draw: a guess that is right
// for most shapes) and the texture memo's slots of the texmaps whose registers changed; at the next call, if nothing changed in
// between, the keys the set names (the registry's copies ResolveKeysCached takes, ~400 bytes for the fragment key) and the cache
// entries of the texture memo. Only reads; the memo and the registries are the player's.
void ResolvePrefetch() {
    if (RsCacheMode() != 1) return;
    static struct { uint64_t h = 0; uint32_t mask = ~0u; uint8_t spo = 0, step = 0, tex = 0, texStep = 0; } pf;
    // ---- texture bindings (the texmaps whose registers changed since the last resolution: the material just replayed)
    const uint8_t tex = g.texSlotDirty;
    if (tex != pf.tex) {
        pf.tex = tex;
        pf.texStep = tex ? 1 : 0;
        if (tex) TexMemoPrefetch(tex, 1);
    } else if (pf.texStep == 1) {
        pf.texStep = 2;
        TexMemoPrefetch(tex, 2);
    }
    // ---- shader keys
    const uint64_t h = KeyHashNow();
    uint32_t mask = 0;
    for (int i = 0; i <= 8; ++i)
        if (g.vcd[i]) mask |= 1u << i;
    const uint8_t spo = (mask & 1u) ? cur.palCls : 0;
    if (g_curKey.valid && cur.valid && g_curKey.h == h && g_curKey.mask == mask && g_curKey.spo == spo) return; // (the keys the draw has)
    if (h != pf.h || mask != pf.mask || spo != pf.spo) {
        pf.h = h;
        pf.mask = mask;
        pf.spo = spo;
        pf.step = 1;
        FePrefetchLines(&g_keyMemo.At(KeyMemoTable::Set(h, mask, spo) * KeyMemoTable::kMaxWays), KeyMemoWays() * sizeof(KeyMemo), 4);
        return;
    }
    if (pf.step != 1) return;
    pf.step = 2;
    uint32_t slot = 0;
    if (const KeyMemo* m = g_keyMemo.Find(h, mask, spo, KeyMemoWays(), slot)) {
        if (m->fsId < g_fsReg.keys.size() && (!cur.valid || m->fsId != cur.fsId)) FePrefetchLines(&g_fsReg.keys[m->fsId], sizeof(FsKey), 16);
        if (m->vsId < g_vsReg.keys.size() && (!cur.valid || m->vsId != cur.vsId)) FePrefetchLines(&g_vsReg.keys[m->vsId], sizeof(VsKey), 8);
    }
}
#include "vgx_direct_draw.inc" // GXV_DIRECT_J3D (vgx_direct.h): the templated resolution of J3D shapes' static runs
#include "vgx_j3native2_draw.inc" // GXV_J3DNATIVE=4 / 5 (vgx_j3n2.h): the templated resolution of a chain's static runs
namespace {

inline __attribute__((always_inline)) void ResolveState(uint32_t vcdMtxMask, uint32_t vertexCount) {
    dj::SyncKeysForResolve(vcdMtxMask); // (GXV_DIRECT_J3D: key copies a templated resolution left undone)
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
uint32_t g_pendingIdxAlt = ~0u, g_pendingIdxAltCount = 0; // (GXV_DLVCACHE)
void SetPendingIdxAlt(uint32_t idxOffset, uint32_t count) { g_pendingIdxAlt = idxOffset; g_pendingIdxAltCount = count; }
facecull::Run* g_pendingFace = nullptr; // (GXV_FACECULL)
void SetPendingFace(facecull::Run* r) { g_pendingFace = r; }
uint32_t g_pendingVq = 0; // (GXV_VTXPACK)
void SetPendingVq(uint32_t vqK) { g_pendingVq = vqK; }
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
    const bool on = VGX_KNOB(EnvInt("GXV_ABSIDX", GXV_ABSIDX_DEFAULT) != 0) != 0;
    return on;
}
static size_t g_chunkStart = 0;    // byte offset in g_buf.vtx of the current chunk
static uint32_t g_pendingFirst = 0; // index of the pending vertices' first one, from the chunk start

// The layout of the mask asked for last (a frame asks for a few masks, tens of thousands of times).
// (a table: register folding asks for two masks in turn for every particle)
static uint32_t g_strideInv[4096]; // ceil(2^32 / stride): n / stride == (n * inv) >> 32 for every byte offset in a chunk
// Which of the matrix-index attributes (PNMTXIDX, TEX0MTXIDX..TEX7MTXIDX) the vertex descriptor has: bit i for vcd[i] != 0 (descriptor
// types are 0..3). The eight bytes in one load, a bit gathered from each by one multiply.
static inline uint32_t VcdMtxMask() {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint64_t x;
    std::memcpy(&x, g.vcd, 8);
    x = (x | (x >> 1)) & 0x0101010101010101ull;
    return static_cast<uint32_t>((x * 0x0102040810204080ull) >> 56) | (g.vcd[8] ? 0x100u : 0u);
#else
    uint32_t m = 0;
    for (int i = 0; i <= 8; ++i)
        if (g.vcd[i]) m |= 1u << i;
    return m;
#endif
}
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
// its memory may only move while that thread has nothing left to do.) Past its floor it grows in steps of
// at most 2 MiB, not by doubling (vgx_arena.h: one request of twice the arena is what a cut-up heap cannot meet).
static inline void ArenaGrow(size_t n) {
    if (n > g_buf.vtx.capacity()) {
        if (split::g_on) split::BeforeArenaGrow(g_buf.vtx, n);
        arena::VtxMakeRoom(g_buf.vtx, n, arena::Get());
    }
    g_buf.vtx.resize(n);
}
// strk26 (GXV_ARENA_SOFT): the same, but a refusal by the heap is not the end of the program: false, the arena untouched (vgx_arena.h VtxTryRoom).
static inline bool ArenaGrowSoft(size_t n) {
    if (n > g_buf.vtx.capacity()) {
        if (split::g_on) split::BeforeArenaGrow(g_buf.vtx, n);
        if (!arena::VtxTryRoom(g_buf.vtx, n, arena::Get())) return false;
    }
    g_buf.vtx.resize(n);
    return true;
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
            if (!ArenaGrowSoft(start)) { RestoreBvSnapshot(); arena::SoftNoteDrop(g_frame); return nullptr; } // (GXV_ARENA_SOFT: no room: this call draws nothing)
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
    if (!ArenaGrowSoft(g_pendingVtxStart + static_cast<size_t>(count) * L.stride)) { RestoreBvSnapshot(); arena::SoftNoteDrop(g_frame); return nullptr; } // (GXV_ARENA_SOFT)
    uint8_t* const out = g_buf.vtx.data() + g_pendingVtxStart;
    // Diagnostic guard (console crash at the first frame after a cutscene skip: a store to 0x002F1080,
    // which is not mapped). The vertices must lie in the arena. Otherwise log the arena's state (a
    // few times) and let the caller draw nothing for this call.
    // (the address test is the console's: its user memory starts at 0x80000000; elsewhere, for example the 32-bit
    // Linux harness, ordinary heap memory lies lower, so only a null base counts)
#if defined(__vita__) || defined(PORT_ARMHF_VITA_SHIM)  // (the armhf PGO harness compiles the console's branch, with its own arena floor)
#ifndef VGX_ARENA_MIN
#define VGX_ARENA_MIN 0x80000000u
#endif
    const bool badArena = reinterpret_cast<uintptr_t>(out) < VGX_ARENA_MIN || g_buf.vtx.data() == nullptr;
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

// GXV_VARFRONT (read once, default 1; 0: off): in front of the two seen-sets, the variants this path itself put into both, by a hash of
// the variant key (direct-mapped, 1,024 slots). A material switch draws with another variant than the last draw (g_lastVariant), and
// each such draw asked both std::unordered_sets, a software division and a chase through a bucket's nodes each on the console, about
// 600 times a picture, to learn that the variant was known. A slot holds a key only after both of its inserts, and the sets are never
// erased, so a key found here is in both and the inserts would do nothing; a miss asks the sets as before.
static constexpr uint32_t kVarFrontSize = 1024;
static uint64_t g_varFront[kVarFrontSize];
static bool g_varFrontInit = false;
static uint64_t g_varFrontHits = 0, g_varFrontMisses = 0;
void VarFrontCounters(uint64_t out[2]) { out[0] = g_varFrontHits; out[1] = g_varFrontMisses; }
static inline bool VarFrontOn() {
    const bool on = VGX_KNOB(EnvInt("GXV_VARFRONT", 1) != 0) != 0;
    return on;
}
static inline uint64_t& VarFrontSlot(uint64_t vk) {
    if (!g_varFrontInit) { // (~0: no key; VariantKey's bits 25..31 are always zero, so no key is ~0)
        for (uint64_t& x : g_varFront) x = ~0ull;
        g_varFrontInit = true;
    }
    uint64_t x = vk ^ (vk >> 29);
    x *= 0x9E3779B97F4A7C15ull;
    return g_varFront[static_cast<uint32_t>(x >> 40) & (kVarFrontSize - 1)];
}

// GXV_ALPHATEST_PREPASS: a record given a prepass key is drawn with up to three more programs / variants than its own: its prepass key with the colour mask off, its colour
// variant, and its tested key (the back end's fallback). The shader list (the next run's warm-up) learns them with it, so that no draw of the pair waits for a compile.
static void PrepassNoteExtras(const split::DrawStateRef& st, uint32_t layoutMask) {
    uint16_t orig = 0, col = 0;
    if (!g_shaderList || !PrepassPair(st.useFs, orig, col)) return;
    RenderState masked = *st.rs;
    masked.colorWrite = 0;
    masked.alphaWrite = 0;
    const uint32_t bkOn = BlendKey(*st.rs), bkOff = BlendKey(masked);
    const struct { uint16_t fs; uint32_t bk; } more[3] = {{st.useFs, bkOff}, {col, bkOn}, {orig, bkOn}};
    for (const auto& m : more) {
        const uint64_t vk = VariantKey(st.vsId, m.fs, layoutMask, m.bk);
        two::AGuard lockedForTheSecondWorker;
        const uint32_t pk = (static_cast<uint32_t>(st.vsId) << 16) | m.fs;
        if (g_progSeen.insert(pk).second) {
            const uint32_t v[2] = {st.vsId, m.fs};
            ListAppend(3u, v, sizeof v);
        }
        if (g_varSeen.insert(vk).second) {
            const uint32_t v[4] = {st.vsId, m.fs, layoutMask & 0xFFFu, m.bk};
            ListAppend(4u, v, sizeof v);
        }
    }
}
// The shader warm-up list: the program and variant of a new record.
void split::NoteVariant(const DrawStateRef& st, uint32_t layoutMask) {
#if defined(VGX_CLONE_B)
    {
        // (GXV_FE_TWOCORE, the second worker: a program or variant not on the player's list yet ends the segment: the player notes it)
        const uint32_t bk = BlendKey(*st.rs);
        const uint64_t vk = VariantKey(st.vsId, st.useFs, layoutMask, bk);
        if (vk == g_lastVariant) return;
        two::LockB();
        const int known = vgx2_variant_known((static_cast<uint32_t>(st.vsId) << 16) | st.useFs, vk);
        vgx2_unlock_b();
        if (!known) two::Abandon(3);
        g_lastVariant = vk;
        return;
    }
#endif
    if (__builtin_expect(PrepassAny(), 0)) PrepassNoteExtras(st, layoutMask);
    if (g_shaderList) {
        const uint32_t bk = BlendKey(*st.rs);
        const uint64_t vk = VariantKey(st.vsId, st.useFs, layoutMask, bk);
        if (vk != g_lastVariant) {
#if GXV_RECIPE_SEED_BATCH
            if (g_recipeMerged) PersistRecipeDraw(st.vsId, st.useFs, layoutMask, bk);
            else {
#endif
            g_lastVariant = vk;
            uint64_t* front = VarFrontOn() ? &VarFrontSlot(vk) : nullptr;
            if (front) {
                if (*front == vk) { ++g_varFrontHits; return; } // (in both sets already)
                ++g_varFrontMisses;
            }
            const uint32_t pk = (static_cast<uint32_t>(st.vsId) << 16) | st.useFs;
            two::AGuard lockedForTheSecondWorker; // (GXV_FE_TWOCORE: it reads both sets)
            if (g_progSeen.insert(pk).second) {
                const uint32_t v[2] = {st.vsId, st.useFs};
                ListAppend(3u, v, sizeof v);
            }
            if (g_varSeen.insert(vk).second) {
                const uint32_t v[4] = {st.vsId, st.useFs, layoutMask & 0xFFFu, bk};
                ListAppend(4u, v, sizeof v);
            }
            if (front) *front = vk;
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
    if (c.rig) c.rig->rec = -1; // (GXV_RIGIDMERGE: a new record is no palette run yet)
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
// `face` (gpuab23, GXV_SHADOW_CULLCOVER): for a rectangle, the winding of its first triangle on the screen (y down): 1 clockwise (a GX
// front face), -1 counter-clockwise (a back face), 0 not worked out. A rectangle's triangles all wind alike (quads and fans: (0 1 2),
// (0 2 3); a strip's second triangle is drawn with its order swapped back), so the whole quad is drawn or culled together.
#include "vgx_fpbox.inc" // (neonbox40: SmallBoundsCore / ChaProjT, the arithmetic as it was with fpb::Active's compares; GALAXY_FPBITS=0 builds the plain float code)
static bool SmallDrawBounds(uint8_t gxPrim, const uint8_t* v, uint32_t count, uint32_t stride, EfbRect& box, bool& rect, int* face = nullptr) {
    const RenderState& rs = cur.rs;
    BoundsState s;
    for (int i = 0; i < 4; ++i) s.p[i] = u2f(g.xfReg[0x20 + i]);
    s.ortho = (g.xfReg[0x26] & 1) != 0;
    s.vpX = rs.vpX; s.vpY = rs.vpY; s.vpW = rs.vpW; s.vpH = rs.vpH;
    s.scX = rs.scX; s.scY = rs.scY; s.scW = rs.scW; s.scH = rs.scH;
    int b[4];
    const bool ok = SmallBoundsCore<fpb::Active>(s, gxPrim, v, count, stride, b, rect, face);
    if (ok) box = EfbRect{static_cast<int16_t>(b[0]), static_cast<int16_t>(b[1]), static_cast<int16_t>(b[2]), static_cast<int16_t>(b[3])};
    return ok;
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
//   GXV_CHARAA_AUTO=1|2    (default 0; with GXV_CHARAA off) the same machinery for the player's own draws only (the textures of the
//                          pack's list from the archives of Mario and Luigi: IsPlayerTexture). =1: the back end draws the filter
//                          only while the picture is upscaled from a render size below the EFB's (backend::PresentCharaAA): Mario
//                          stays crisp exactly when the rest of the picture softens. =2: whenever the picture is shown larger than it
//                          was drawn and the EFB is not multisampled, so at 640x456 too (the 960x544 screen shows it upscaled; the
//                          area cap GXV_CHARAA_AREA applies as ever). The Observatory's 190 Luma draws are not
//                          noted, so the 0.66 ms measured for all characters is not paid. HUD markers are written, as for GXV_CHARAA=1.
//   mario24 (the owner, 11 Nov: "Mario looks a little soft at 1.0x"; agents/mario24/REPORT.txt): the same noting of the player's draws, for the present's
//   own pass over the player's rectangle (backend::PresentFastDraw, after the whole-picture edge smoothing, before the HUD is drawn plainly again):
//   GXV_EDGE_AA_PLAYER=0|1|2   (default 0: nothing, the stream and the picture exactly as before) 1: the rectangle is left out of the edge smoothing
//                              (the plain present's pixels: no blend along edges inside it); 2: the edge smoothing there is lighter (a higher
//                              threshold, GXV_EDGE_AA_PLAYER_T per mille, default 250, and the diagonal edges left alone)
//   GXV_PLAYER_SHARPEN=<pct>   (default 0) an unsharp mask over the same rectangle with either of the above (alone it acts as =1): the present's
//                              own form, c + (c - mean of the neighbours) x pct / 100 (the upscaled present uses 0.3 .. 0.8; at 960x544 it uses 0)
//   Both need GXV_EDGE_AA=1 and GXV_PRESENT_FAST=1 (the Vita's defaults) and a pack that names the player's textures; the rectangle is the projected bounds
//   of the player's draws as for GXV_CHARAA (CopyDispCmd::player), so it takes in some background around him.
bool PlayerRectsEnv() {
    const bool on = VGX_KNOB(EnvInt("GXV_EDGE_AA_PLAYER", 0) > 0 || EnvInt("GXV_PLAYER_SHARPEN", 0) > 0) != 0;
    return on;
}
// GXV_CHARAA / GXV_CHARAA_AUTO ask for the character filter itself (the rectangles go to CopyDispCmd::chara), apart from the player's own pass above.
bool CharaFilterWanted() {
    const bool on = VGX_KNOB(EnvInt("GXV_CHARAA", kCharaDefault) != 0 || EnvInt("GXV_CHARAA_AUTO", 0) != 0) != 0;
    return on;
}
int CharaAAMode() {
    const int m = static_cast<int>(VGX_KNOB([] {
        const long v = EnvInt("GXV_CHARAA", kCharaDefault);
        const int mode = v < 0 ? 0 : (v > 2 ? 2 : static_cast<int>(v));
        return mode == 0 && (EnvInt("GXV_CHARAA_AUTO", 0) != 0 || PlayerRectsEnv()) ? 1 : mode;
    }()));
    return m;
}
// GXV_CHARAA_AUTO (or the player's pass above) in force: only the player's draws are noted (the switch itself being off).
bool CharaAAPlayerOnly() {
    const bool on = VGX_KNOB(EnvInt("GXV_CHARAA", kCharaDefault) <= 0 && (EnvInt("GXV_CHARAA_AUTO", 0) != 0 || PlayerRectsEnv())) != 0;
    return on;
}
// ================================================================ whole-picture edge smoothing (GXV_EDGE_AA)
// The user, 5 Nov 2026, on Locked 30 (GXV_FPS_LIMIT=30, which starts at 960x544): "jagged edges, no AA". At 960x544 the
// present is a plain 1:1 blit, and the multisampled rungs (7, 8) are never reached in the lit Observatory (the GPU's ~29 ms is
// over the 70 % of 33 ms a step up must fit). So the present itself smooths the whole picture: one quad with an FXAA 3.11
// "console" filter in place of the blit (backend::PresentEdgeAA; no render target, no scene, no copy added). The game's HUD is
// in the EFB by the display copy and must not be filtered, so the front end keeps what CHARAA already finds: every HUD draw
// (between PORT_GX_MARK_HUD_BEGIN / END) that can change a pixel gives its rectangle, padded by kChaPad for the filter's reach and
// merged into at most kCharaBoxes; the back end draws those rectangles again, plainly, over the smoothed picture.
// Front-end cost: the HUD draws' bounds only (a few dozen draws a picture, each a handful of projected points or its list's
// row spheres). The character noting of GXV_CHARAA is not done for this switch (the 0.66 ms of the 24 Oct A/B); with
// GXV_CHARAA_AUTO also on (the Vita's default) both run, and a copy smoothed whole does not draw the character pass on top.
//   GXV_EDGE_AA=0|1|2|3   off (default; the stream, the commands and the present as before) / on / on with the pixels the filter
//                         changed tinted red (diagnostic) / on for the left half of the screen only (same-picture A/B for screenshots)
//   GXV_EDGE_AA_HUD=<pct> most of the display copy the HUD rectangles may cover (default 50); above it the copy is presented
//                         as before (a menu over the picture: filtering, then covering most of it again, would cost for nothing)
// Back-end switches (vgx_backend_gl.cpp, EdgeParams): GXV_EDGE_AA_THRESHOLD, GXV_EDGE_AA_MIN, GXV_EDGE_AA_SHARPNESS.
int EdgeAAMode() {
    const int m = static_cast<int>(VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_EDGE_AA", 0), 0, 3))));
    return m;
}
namespace {
inline bool ChaOn() {
    const bool on = VGX_KNOB(CharaAAMode() != 0 || EdgeAAMode() != 0 || HrwMode() == 5) != 0; // (GXV_EDGE_AA: the HUD's rectangles; GXV_HRW=5: the warp leaves them alone)
    return on;
}
inline bool EdgeOn() {
    const bool on = VGX_KNOB(EdgeAAMode() != 0) != 0;
    return on;
}
inline bool HudRectOn() { // (the HUD rectangles are collected: GXV_EDGE_AA, or GXV_HRW=5 for the warp, which does not turn the edge pass on)
    return EdgeOn() || HrwMode() == 5;
}
inline bool CharaNoteOn() { // (characters are noted for GXV_CHARAA / GXV_CHARAA_AUTO only, not for GXV_EDGE_AA)
    const bool on = VGX_KNOB(CharaAAMode() != 0) != 0;
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
    EfbRect edgeHud[kChaHud]; // GXV_EDGE_AA: the HUD draws' rectangles of this picture (unpadded)
    int ne = 0;
} g_cha;
int g_hudnDepth = 0; // PORT_GX_MARK_HUDN_BEGIN / END (nesting; GXV_HUD_NATIVE)
// GXV_EDGE_AA totals since the start (EdgeCounters): [0] display copies, [1] of them smoothed whole, [2] HUD rectangles handed to
// the back end, [3] copies not smoothed because the HUD covered more than GXV_EDGE_AA_HUD, [4] HUD draws measured.
uint64_t g_edgeCtr[5] = {};
uint64_t g_hudClearedN = 0;   // (14 Nov c162, GXV_HUD_RECT_LOG: HUD rectangles dropped because an EFB copy with a clear covered them)
EfbRect g_hudClearedSrc{};
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
struct ChaProj : ChaProjT<fpb::Active> { // (neonbox40: the arithmetic is vgx_fpbox.inc's)
    ChaProj() {
        for (int i = 0; i < 4; ++i) p[i] = u2f(g.xfReg[0x20 + i]);
        ortho = (g.xfReg[0x26] & 1) != 0;
        vx = cur.rs.vpX; vy = cur.rs.vpY; vw = cur.rs.vpW; vh = cur.rs.vpH;
    }
    bool Rect(EfbRect& r) const {
        const RenderState& rs = cur.rs;
        const int sc[4] = {rs.scX, rs.scY, rs.scW, rs.scH};
        int o[4];
        if (!ChaProjT<fpb::Active>::Rect(sc, o)) return false;
        r = EfbRect{static_cast<int16_t>(o[0]), static_cast<int16_t>(o[1]), static_cast<int16_t>(o[2]), static_cast<int16_t>(o[3])};
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
#if defined(VGX_CLONE_B)
    two::NoteChaBox(r.x, r.y, r.w, r.h); // (GXV_FE_TWOCORE: the player adds it, in order, when it takes the segment)
    return;
#endif
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
// Whether the current state's textures make a character's draw (g_cha.chara), worked out again when the state changed.
void ChaClassify() {
    if (cur.stateSerial != g_cha.serial) {
        g_cha.serial = cur.stateSerial;
        bool c = false;
        const bool playerOnly = VGX_KNOB(CharaAAPlayerOnly()) != 0;
        for (uint32_t m = cur.texMask; m && !c; m &= m - 1) {
            const uint32_t id = cur.tex[__builtin_ctz(m)].texId;
            c = playerOnly ? IsPlayerTexture(id) : IsCharaTexture(id);
        }
        g_cha.chara = c;
    }
}
// (GXV_SPLIT_COPY) Whether CharaNote may read the draw's vertices (conservative: true when unsure), so that EmitDraw copies a
// display-list cache segment on the player first. A HUD draw may; a character's draw does unless its list gave bounds.
bool CharaNoteReads() {
    if (g_cha.hudDepth > 0) return true;
    if (!CharaNoteOn()) return false;
    ChaClassify();
    return g_cha.chara && cur.rs.colorWrite && !(g_cha.pend && g_cha.pendN);
}
void CharaNote(const uint8_t* v, uint32_t count, uint32_t stride, uint32_t layoutMask) {
    const RowBox* pb = g_cha.pend;
    const uint32_t pn = g_cha.pendN;
    g_cha.pend = nullptr;
    g_cha.pendN = 0;
#if defined(VGX_CLONE_B)
    if (g_cha.hudDepth > 0) two::Abandon(7); // (GXV_FE_TWOCORE: never begun inside the HUD markers, which end a segment)
#endif
    if (g_cha.hudDepth > 0) {
        const bool edge = HudRectOn(); // (GXV_EDGE_AA: every HUD draw counts, the whole picture being smoothed)
        if ((g_cha.n == 0 && !edge) || !cur.rs.colorWrite) return; // (no rectangle to cut / the picture's colour is not written)
        if (g_cha.n) ++g_chaCtr[4];
        if (edge) ++g_edgeCtr[4];
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
        bool charaCut = g_cha.n != 0, edgeCut = edge;
        if (cur.texMask == 0 && !(layoutMask & (VL_COL0 | VL_COL1)) && v) {
            // (GXV_EDGE_AA: the same rule against the display copy's own rectangle, unpadded: the fade quad is cut to the scissor)
            if (edge && RInside(g.dispCopySrc, r)) edgeCut = false;
            bool all = true;
            for (int i = 0; i < g_cha.n && all; ++i) {
                const EfbRect& b = g_cha.box[i];
                all = RInside(EfbRect{static_cast<int16_t>(b.x - kChaPad - 2), static_cast<int16_t>(b.y - kChaPad - 2), static_cast<int16_t>(b.w + 2 * kChaPad + 4),
                                      static_cast<int16_t>(b.h + 2 * kChaPad + 4)}, r);
            }
            if (all) charaCut = false;
            if (!charaCut && !edgeCut) return;
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
        if (charaCut) ChaAddHud(r);
        if (edgeCut && r.w > 0 && r.h > 0) {
            if (g_cha.ne < kChaHud) g_cha.edgeHud[g_cha.ne++] = r;
            else g_cha.edgeHud[kChaHud - 1] = RUnion(g_cha.edgeHud[kChaHud - 1], r); // (conservative: restores more)
        }
        return;
    }
    if (!CharaNoteOn()) return; // (GXV_EDGE_AA alone: no character is noted)
    ChaClassify();
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
    for (int i = 0; i < g_cha.ne;) // (GXV_EDGE_AA: a HUD drawn into an offscreen picture is not on the display copy)
        if (RInside(g_cha.edgeHud[i], src)) { g_cha.edgeHud[i] = g_cha.edgeHud[--g_cha.ne]; ++g_hudClearedN; g_hudClearedSrc = src; }
        else ++i;
}
// GXV_EDGE_AA at the display copy: the HUD rectangles, padded by kChaPad (the filter reads its neighbours; a scene pixel next to the
// HUD comes out plain too), cut to the copy, merged where they meet (8 px of slack: a line of text is one rectangle) and then, while
// there are more than the command holds, the two whose union grows least. Merging only ever restores more pixels plainly.
void EdgeFinish(CopyDispCmd& c) {
    const int64_t maxPct = static_cast<int64_t>(VGX_KNOB(std::clamp<long>(EnvInt("GXV_EDGE_AA_HUD", 50), 1, 100)));
    c.edge = false;
    c.hudN = 0;
    c.hudState = 0;
    ++g_edgeCtr[0];
    EfbRect w[kChaHud];
    int n = 0;
    for (int i = 0; i < g_cha.ne; ++i) {
        const EfbRect& b = g_cha.edgeHud[i];
        const int x0 = std::max<int>(b.x - kChaPad, c.src.x), y0 = std::max<int>(b.y - kChaPad, c.src.y);
        const int x1 = std::min<int>(b.x + b.w + kChaPad, c.src.x + c.src.w), y1 = std::min<int>(b.y + b.h + kChaPad, c.src.y + c.src.h);
        if (x1 > x0 && y1 > y0) w[n++] = EfbRect{static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(x1 - x0), static_cast<int16_t>(y1 - y0)};
    }
    g_cha.ne = 0;
    for (bool again = n > 1; again;) {
        again = false;
        for (int i = 0; i < n && !again; ++i)
            for (int j = i + 1; j < n && !again; ++j) {
                const EfbRect &a = w[i], &b = w[j];
                if (a.x <= b.x + b.w + 8 && b.x <= a.x + a.w + 8 && a.y <= b.y + b.h + 8 && b.y <= a.y + a.h + 8) {
                    w[i] = RUnion(a, b);
                    w[j] = w[--n];
                    again = true;
                }
            }
    }
    while (n > kCharaBoxes) {
        int bi = 0, bj = 1;
        int64_t grow = INT64_MAX;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                const int64_t d = RArea(RUnion(w[i], w[j])) - RArea(w[i]) - RArea(w[j]);
                if (d < grow) { grow = d; bi = i; bj = j; }
            }
        w[bi] = RUnion(w[bi], w[bj]);
        w[bj] = w[--n];
    }
    int64_t area = 0;
    for (int i = 0; i < n; ++i) area += RArea(w[i]);
    if (area * 100 > RArea(c.src) * maxPct) {
        ++g_edgeCtr[3];
        c.hudState = 2;
        // (warp32 PATCH13: the rectangles are handed over all the same, for GXV_HRW=5's warp, which redraws them up to its own limit instead of giving the pair up;
        // console c118 Dreadnought: every pair fell back here. The first three such pictures are named in the log.)
        for (int i = 0; i < n; ++i) c.hud[i] = w[i];
        c.hudBigN = static_cast<uint8_t>(n);
        c.hudBigPct = static_cast<uint8_t>(std::min<int64_t>(100, RArea(c.src) > 0 ? area * 100 / RArea(c.src) : 100));
        static int logged = 0;
        if (logged < 3 && port_log_summary) {
            ++logged;
            char line[400];
            int o = std::snprintf(line, sizeof line, "hud rectangles: over %d %% of the picture (%d %%, %d rectangles after merging; x,y wxh in EFB units):", static_cast<int>(maxPct), static_cast<int>(c.hudBigPct), n);
            for (int i = 0; i < n && i < 8 && o > 0 && o < static_cast<int>(sizeof line) - 28; ++i)
                o += std::snprintf(line + o, sizeof line - static_cast<size_t>(o), " %d,%d %dx%d", w[i].x, w[i].y, w[i].w, w[i].h);
            port_log_summary("%s", line);
        }
        return;
    }
    for (int i = 0; i < n; ++i) c.hud[i] = w[i];
    c.hudN = static_cast<uint8_t>(n);
    c.hudState = 1;
    {   // (14 Nov c162) GXV_HUD_RECT_LOG=<n> (default 0): every n-th display copy, one line with its HUD rectangles (EFB units) and the rectangles dropped by a clearing copy
        // since the line before (the owner's lives-counter flicker at the bottom-left: which rectangles the warp's HUD layer gets)
        static const long every = std::clamp<long>(EnvInt("GXV_HUD_RECT_LOG", 0), 0, 100000);
        static long tick = 0;
        if (every > 0 && port_log_summary && ++tick >= every) {
            tick = 0;
            char line[480];
            int o = std::snprintf(line, sizeof line, "hud-rects: picture %dx%d at %d,%d: %d rectangles (x,y wxh):", c.src.w, c.src.h, c.src.x, c.src.y, n);
            for (int i = 0; i < n && o > 0 && o < static_cast<int>(sizeof line) - 40; ++i)
                o += std::snprintf(line + o, sizeof line - static_cast<size_t>(o), " %d,%d %dx%d", w[i].x, w[i].y, w[i].w, w[i].h);
            if (o > 0 && o < static_cast<int>(sizeof line) - 80)
                std::snprintf(line + o, sizeof line - static_cast<size_t>(o), " | dropped by a clearing copy %u (last copy %d,%d %dx%d)", static_cast<unsigned>(g_hudClearedN),
                              g_hudClearedSrc.x, g_hudClearedSrc.y, g_hudClearedSrc.w, g_hudClearedSrc.h);
            g_hudClearedN = 0;
            port_log_summary("%s", line);
        }
    }
    c.edge = EdgeOn(); // (HudRectOn() alone, GXV_HRW=5: the rectangles are there, the present's edge pass is not asked for)
    ++g_edgeCtr[1];
    g_edgeCtr[2] += static_cast<uint64_t>(n);
}
void CharaFinish(CopyDispCmd& c) {
    const int64_t maxPct = static_cast<int64_t>(VGX_KNOB(std::clamp<long>(EnvInt("GXV_CHARAA_AREA", 30), 1, 100)));
    ++g_chaCtr[0];
    g_cha.hudDepth = 0;
    g_hudnDepth = 0;
    c.charaN = 0;
    c.playerN = 0;
    if (HudRectOn()) EdgeFinish(c);
    if (g_cha.n == 0) { g_cha.nh = 0; return; }
    ++g_chaCtr[1];
    EfbRect work[kChaWork], next[kChaWork];
    int nw = 0;
    // GXV_CHARAA_MIN=<px> (default 32): a rectangle less tall than this many EFB pixels is not smoothed. A character that
    // small (a far Luma, a Star Bit-sized item) is all texture detail and few stair steps, and the filter would only soften
    // it (desktop captures, 23 Oct: a small Mario's face and overalls came out blurred with FXAA's looser thresholds).
    const int minH = static_cast<int>(VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_CHARAA_MIN", 32), 0, 456))));
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
    if (CharaFilterWanted()) {
        for (int i = 0; i < nw; ++i) c.chara[i] = work[i];
        c.charaN = static_cast<uint8_t>(nw);
        g_chaCtr[5] += static_cast<uint64_t>(nw);
        g_chaCtr[6] += static_cast<uint64_t>(area);
    }
    if (PlayerRectsEnv()) { // (mario24: the same rectangles for the present's own pass over the player; with GXV_CHARAA_AUTO and this both on, both get them)
        for (int i = 0; i < nw; ++i) c.player[i] = work[i];
        c.playerN = static_cast<uint8_t>(nw);
    }
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
// GXV_HUD_NATIVE (hud24; the back end's part: vgx_backend_gl.cpp "namespace hudn"): 0 off, 1 on, 2 plan and count only. With it on, every draw made
// between the game's 2D markers (PORT_GX_MARK_HUD_BEGIN/END, and HUDN_BEGIN/END for the scenes that draw their layouts outside those) is flagged
// (DrawRec::hud), and no record merges with one of the other kind. Off: no flag, no merge test result changes, the markers are not even written.
int HudNativeMode() {
    const int m = static_cast<int>(PORT_STAGE_KNOB(int32_t, static_cast<int>(std::clamp<long>(port::stageknob::Int("GXV_HUD_NATIVE", 0), 0, 2))));
    return m;
}
namespace {
// GXV_HRW (hrw27): the owner the game's last PORT_GX_MARK_OWNER named (vgx_hrw.h OwnerKey: id << 8 | phase; 0 none). Every draw made while it is not 0 carries it (DrawJob::owner, DrawRec::owner),
// and no record merges with one of another owner. Written by the front end's one parsing thread; reset at a picture's end and at a partial draw's end.
uint16_t g_ownerKey = 0;
int g_playerMarkDepth = 0; // g30: nesting depth of PORT_GX_MARK_PLAYER_BEGIN (5) / _END (6)
// The job's cover bit 2 (value 4): this draw is 2D.
inline uint8_t HudJobBit() {
    return ((HudNativeMode() != 0 || HrwMode() != 0) && (g_cha.hudDepth > 0 || g_hudnDepth > 0)) ? 4u : 0u; // (GXV_HRW: the replay never re-projects a draw made between the HUD markers)
}
} // namespace
bool g_transientNext = false; // (GXV_INDCAP_SKIP: PORT_GX_MARK_TRANSIENT_COPY seen; the next EFB copy is the game's "Indirect" capture)
uint32_t g_transientStage = 0; // (GXV_INDCAP_SKIP: the stage tag of the last marker: a hash of the stage name and scenario, 0 none)
void TransientMarker(uint32_t stage) {
    g_transientNext = true;
    g_transientStage = stage;
}
void CharaMarker(uint32_t kind) {
    if (kind == 10) { g_transientNext = true; return; }                 // PORT_GX_MARK_TRANSIENT_COPY (GXV_INDCAP_SKIP)
    if (kind == 5) { J3n2Flush(); ++g_playerMarkDepth; return; }        // PORT_GX_MARK_PLAYER_BEGIN (g30: asynclink chara protection)
    if (kind == 6) { J3n2Flush(); if (g_playerMarkDepth > 0) --g_playerMarkDepth; return; } // PORT_GX_MARK_PLAYER_END
    if (kind == 12) { ++g_hudnDepth; return; }                          // PORT_GX_MARK_HUDN_BEGIN (GXV_HUD_NATIVE)
    if (kind == 13) { if (g_hudnDepth > 0) --g_hudnDepth; return; }     // PORT_GX_MARK_HUDN_END
    if (kind == 1) ++g_cha.hudDepth;                                   // PORT_GX_MARK_HUD_BEGIN
    else if (kind == 2 && g_cha.hudDepth > 0) --g_cha.hudDepth;         // PORT_GX_MARK_HUD_END
}
void PresentationMarker(uint32_t kind) {
    g_buf.presentAllowed = kind != kPresentSuppressedMarker;
}
void PresentationStamp(uint32_t vblank16) {
    g_buf.presentStamp = 0x80000000u | (vblank16 & 0xFFFFu);
}
// GXV_HRW (hrw26): 0 off (default), 1 on, 2 verify-still, 3 census (vgx_hrw.h). Read once.
int HrwMode() {
    return VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_HRW", 0), 0, 5)));
}
void HrwMarker(uint32_t token) {
    g_buf.setHrwPair(token);
}
// GXV_HRW (hrw27): PORT_GX_MARK_OWNER (kOwnerMarker) with the key in bits 8-23, 0 = the owner's draws are over; only the half-rate-world modes that keep or compare pictures act on it.
void OwnerMarker(uint32_t key16) {
    J3n2Flush(); // (GXV_J3DNATIVE=4 / 5: a chain still active ends here, so that its draws carry the owner they were sent under)
    g_ownerKey = HrwMode() != 0 ? static_cast<uint16_t>(key16 & 0xFFFFu) : static_cast<uint16_t>(0);
}
// PORT_GX_MARK_FRESH_END (kFreshEndMarker) with the pair's number in bits 8-23: the game's partial draw for the pair's second picture is over. The buffer made so far goes to the render
// thread as a replay job (FrontFresh); the host's hook finishes the split records first, as it does at a display copy.
void FreshEndMarker(uint32_t id16) {
    J3n2Flush();
    g_ownerKey = 0;
    g_playerMarkDepth = 0;
    if (Hooks().freshEnd) Hooks().freshEnd(id16 & 0xFFFFu);
    else FrontFresh(id16 & 0xFFFFu);
}
void CharaCounters(uint64_t out[11]) { std::memcpy(out, g_chaCtr, sizeof g_chaCtr); }
void EdgeCounters(uint64_t out[5]) { std::memcpy(out, g_edgeCtr, sizeof g_edgeCtr); }

// Register folding on a run of particles: every one of them used to lay its vertices out again with two colour
// slots (FoldRegs), hundreds of instructions for a quad. FoldGuess says, before the vertices are decoded, whether
// the draw will fold, from the state ResolveState left and the registers written since: while no shader key, no
// vertex descriptor and no matrix-index mode has changed, the only things that decide it are the vertex count
// and the two TEV registers' values. A draw that follows such a draw is decoded into the folded layout at once.
// (If it does not fold after all, EmitDraw gives the reservation back and the caller decodes again.)
uint32_t FoldGuess(uint32_t layoutMask, uint32_t count) {
    const bool on = VGX_KNOB(EnvInt("GXV_FOLDAHEAD", 1) != 0) != 0;
    const uint32_t slots = VL_COL0 | VL_COL1;
    if (!on || (layoutMask & slots) == slots) return layoutMask; // (the slots are there)
    // (ResolveState rebuilds the keys when any of these is set: no prediction then)
    if (!cur.valid || g.bpKeyDirty || g.xfKeyDirty || g.xfRegDirty) return layoutMask;
    // (the descriptor as ResolveState saw it, and no matrix index per vertex: that case has the short palette test)
    if (g.vcdGen != cur.vcdGen || (cur.vcdMtxMask & 1u)) return layoutMask;
    if (count > SoftXformLimit() || !cur.foldOk || (dj::SyncKeys(), !SoftEligible(cur.vs)) || !FoldValuesOk()) return layoutMask;
    return layoutMask | slots;
}

// ================================================================ rigid draws in one record (GXV_RIGIDMERGE)
// Nine in ten of the game's shapes are rigid: one position matrix for the whole shape. Each is its own draw call when its model-view
// block differs from its neighbour's (the u_mv uniform), even if everything else (shaders, textures, render state, every other block)
// is the same. A run of such draws is made one record with the matrix palette shader the skinned meshes use: every draw of the run
// takes a slot of the palette (three position rows and the three normal rows of its u_mv block) and writes the slot's row number into
// the w of its vertices (where a skinned vertex has its PNMTXIDX), so that the vertex shader reads its own matrix.
//   GXV_RIGIDMERGE=0   (default) off, every key, record and block as before
//   GXV_RIGIDMERGE=1   runs of up to 10 draws (the short palette, 60 vec4: the vertex key with perVtxPos 2)
//   GXV_RIGIDMERGE=2   runs of up to 4 draws (the small palette, 24 vec4: perVtxPos 3)
// Why the picture is the same, bit for bit: the palette vertex shader and the u_mv one are generated from the same lines
// (vgx_shadergen.cpp): eye = (dot(row0, p), dot(row1, p), dot(row2, p)) with p = (x, y, z, 1) in both, the normal from the three normal
// rows in the same order, then the same normalisation; the palette index is the whole number the front end stored (RowIndex), not clamped
// (3 * slot <= 27, or 9). Everything else the shader reads (projection, texture matrices, lights, TEV) comes from blocks that a run shares
// by construction: a draw joins only when all of them are the record's (same offsets in the deduplicating arena). Only the draw that
// follows a record joins it: the order of the triangles, and so of the depth and blend operations, is that of the draws. A draw alone
// keeps its u_mv key and block (the record is converted when the second draw joins it).
// The vertices of a draw are in the frame's arena (static display lists are not touched: their vertices are in a buffer of their own).
namespace {
long RigidMode() {
    const long m = static_cast<long>(VGX_KNOB(EnvInt("GXV_RIGIDMERGE", 0))); // read once, default 0 (off)
    return (m == 1 || m == 2) && UniformPacking() ? m : 0;
}
constexpr uint32_t kRigidMaxSlotsOf[3] = {0, 10, 4};
std::vector<uint16_t> g_rigTwin; // per vertex shader id: the id of its palette twin, 0xFFFF none yet, 0xFFFE not possible
split::RigidRun g_rig;
uint64_t g_rigJoins = 0, g_rigRuns = 0; // draws that joined a palette run, runs made (written by the thread that finishes records)
// The vertex shader a rigid draw of the current key joins a palette run with (the same key with the palette instead of u_mv).
uint16_t RigidTwin() {
    const long mode = RigidMode();
    if (!mode || cur.vs.perVtxPos != 0) return 0xFFFF;
    for (int i = 0; i < cur.vs.numTexGens; ++i)
        if (cur.vs.tg[i].perVtxMtx) return 0xFFFF; // (those read the palette for texture matrices already)
    if (g_rigTwin.size() <= cur.vsId) g_rigTwin.resize(static_cast<size_t>(cur.vsId) + 64, 0xFFFF);
    uint16_t& t = g_rigTwin[cur.vsId];
    if (t == 0xFFFF) {
        VsKey k = cur.vs;
        k.perVtxPos = mode == 1 ? 2 : 3;
        t = Lookup(g_vsReg, k, false, GenerateVertexShader);
    }
    return t;
}
// Whether the draw `j` (state st) can join the record `p` (the last one, index `last`), and if it can, does it. The tests are those of an
// ordinary merge except the vertex shader and the matrix block; `common` is the rest of them.
// dry: only whether it would (GXV_RIGIDMERGE_DLLEARN: a static job, asked as if its list were a copy in the arena; nothing is changed).
template <bool dry, class Common>
bool RigidJoin(split::FinishCtx& c, DrawRec& p, int last, const split::DrawJob& j, const split::DrawStateRef& st, Common common) {
    if (!c.uni || !c.rig || j.vsTwin == 0xFFFF || (!dry && j.kind != split::JOB_DYNAMIC)) return false;
    const long mode = RigidMode();
    if (!mode) return false;
    const bool conv = c.rig->rec == last;
    if (conv ? p.vsId != j.vsTwin : p.vsId != st.vsId) return false;
    if (c.blockOff[UB_MV] == kNoBlock || p.ublock[UB_MV] == kNoBlock) return false;
    for (int b = 0; b < UB_COUNT; ++b) {
        if (b == UB_MV || (b == UB_PAL && conv)) continue; // (the record's own palette replaces them)
        if (p.ublock[b] != c.blockOff[b]) return false;
    }
    UniArena& uni = *c.uni;
    const size_t xo = c.blockOff[UB_MV];
    if (xo + 24 > uni.size() || (!conv && static_cast<size_t>(p.ublock[UB_MV]) + 24 > uni.size())) return false;
    const uint32_t K = mode == 1 ? 30u : 12u, maxSlots = kRigidMaxSlotsOf[mode];
    if (!common()) return false;
    if (dry) {
        if (!conv) return maxSlots >= 2;
        const float* const pl = uni.data() + c.rig->pal;
        const uint32_t nw = c.rig->slots - 1;
        const float* const xx = uni.data() + xo;
        return c.rig->slots < maxSlots || (std::memcmp(pl + nw * 12, xx, 12 * sizeof(float)) == 0 && std::memcmp(pl + K * 4 + nw * 12, xx + 12, 12 * sizeof(float)) == 0);
    }
    // ---- it joins
    uint32_t slot;
    if (!conv) {
        if (maxSlots < 2) return false;
        const size_t o = uni.size();
        uni.resize(o + static_cast<size_t>(K) * 8);
        float* pal = uni.data() + o;
        std::memset(pal, 0, static_cast<size_t>(K) * 8 * sizeof(float));
        const float* const l = uni.data() + p.ublock[UB_MV];
        std::memcpy(pal, l, 12 * sizeof(float));
        std::memcpy(pal + K * 4, l + 12, 12 * sizeof(float));
        p.vsId = j.vsTwin;
        p.ublock[UB_PAL] = static_cast<uint32_t>(o);
        c.rig->rec = last;
        c.rig->slots = 1;
        c.rig->pal = static_cast<uint32_t>(o);
        if (!c.vtxLog) ++g_rigRuns;
    }
    const float* const x = uni.data() + xo; // its u_mv block: position rows 0..2 (4 floats each), normal rows 0..2 (4 floats each)
    float* const pal = uni.data() + c.rig->pal;
    const uint32_t newest = c.rig->slots - 1;
    // (the same matrix as the newest slot's, whole words: no new slot)
    if (std::memcmp(pal + newest * 12, x, 12 * sizeof(float)) == 0 && std::memcmp(pal + K * 4 + newest * 12, x + 12, 12 * sizeof(float)) == 0) {
        slot = newest;
    } else if (c.rig->slots < maxSlots) {
        slot = c.rig->slots++;
        std::memcpy(pal + slot * 12, x, 12 * sizeof(float));
        std::memcpy(pal + K * 4 + slot * 12, x + 12, 12 * sizeof(float));
    } else {
        return false; // full: only possible for the join that opened the run (conv false) when maxSlots < 2, handled above; a full run starts a new record
    }
    const float w = static_cast<float>(3 * slot);
    if (c.vtxLog) {
        c.vtxLog->push_back(split::VtxWrite{j.vtxStart, j.count, j.stride, w});
    } else {
        uint8_t* v = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(j.vtxPtrLo | (static_cast<uint64_t>(j.vtxPtrHi) << 32)));
        for (uint32_t i = 0; i < j.count; ++i, v += j.stride) std::memcpy(v + 12, &w, 4);
    }
    if (!c.vtxLog) ++g_rigJoins;
    return true;
}
// ---- static instances (GXV_STATIC_INST, fe23c, 8 Nov 2026)
// The flower groups' plants and the like (DrawBufferShapeDrawer: one material list for all actors of a model, then per actor its matrices
// and the shape's CALL_DL) reach the record stage as consecutive static runs of one list's buffer that differ only in their u_mv block.
// A static record cannot take another draw (its vertices are its buffer's, not the arena's), so each became a record of its own: one
// GL draw each in the back end (~17-20 us of core 1 a draw) and on the GPU. With the switch on, the cache gives a list it sees drawn so
// several times in a row an instance buffer (vgx_dlcache.cpp "instance buffers": the run's vertices K times, copy k with position w =
// 3k, the palette slot; its indices K times, copy k's + k * vertices), and the jobs of its runs carry that buffer (DrawJob::chunk) and the
// palette twin of their vertex shader (RigidTwin, as GXV_RIGIDMERGE's dynamic draws). Here a job joins the record before it when that
// record is the same run of the same list (or a run of instances of it opened by such a join), drawn with the same fragment shader,
// render state, textures and uniform blocks but u_mv: the record becomes a draw of copies 0..n-1 of the instance buffer with the
// palette in place of u_mv (slot k = draw k's u_mv, written whole, so the vertex shader reads the same rows the separate draws read).
// Exact as GXV_RIGIDMERGE is (its palette shader does the same dot products with the same rows; the same vertices are drawn in the
// same order: copy k is draw k's vertices byte for byte but for w, which only the palette shader reads); tools/test_vgx_staticinst_gl.sh
// draws it on Mesa against the switch off.
// (counters, written by the thread that finishes records one at a time, read by the cache's report line: relaxed loads and stores)
std::atomic<uint32_t> g_instRuns{0}, g_instJoins{0}, g_instRefused{0};
inline void Bump(std::atomic<uint32_t>& a) { a.store(a.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }
bool StaticInstJoin(split::FinishCtx& c, const split::DrawJob& j, const split::DrawStateRef& st) {
    const long mode = RigidMode();
    if (!mode || !c.uni || !c.rig || j.vsTwin == 0xFFFF || j.prim != PRIM_TRIANGLES) return false;
    std::vector<DrawRec>& draws = *c.draws;
    const int last = *c.lastDraw;
    const bool prevIsDraw = c.cmds ? (!c.cmds->empty() && c.cmds->back().type == CMD_DRAW) : j.prevIsDraw != 0;
    if (!(last >= 0 && last == static_cast<int>(draws.size()) - 1 && prevIsDraw)) return false;
    DrawRec& p = draws[last];
    if (p.hud != ((j.cover >> 2) & 1u) || p.owner != j.owner || p.depthMark != j.depthMark || p.playerMark != j.playerMark) return false;
    const uint32_t ib = j.chunk, maxSlots = kRigidMaxSlotsOf[mode], K = mode == 1 ? 30u : 12u;
    const bool conv = c.rig->rec == last && p.vbuf == ib;
    if (conv) {
        // (a run of instances of this list's run, opened by a join below)
        if (p.vsId != j.vsTwin || c.rig->slots >= maxSlots || p.vtxCount != c.rig->slots * j.count || p.idxCount != c.rig->slots * j.idxCount) {
            if (!c.vtxLog) Bump(g_instRefused);
            return false;
        }
    } else if (p.vbuf != j.vbuf || p.vtxOffset != j.vtxOffset || p.vtxFirst != j.vtxFirst || p.idxOffset != j.idxOffset || p.vtxCount != j.count ||
               p.idxCount != j.idxCount || p.vsId != st.vsId || maxSlots < 2) {
        return false; // (not the same run of the same buffer: nothing to do with instances)
    }
    if (p.prim != j.prim || p.layoutMask != j.layoutMask || p.fsId != st.useFs) return false;
    if (c.blockOff[UB_MV] == kNoBlock || (!conv && p.ublock[UB_MV] == kNoBlock)) return false;
    for (int b = 0; b < UB_COUNT; ++b) {
        if (b == UB_MV || (b == UB_PAL && conv)) continue; // (the record's own palette replaces them)
        if (p.ublock[b] != c.blockOff[b]) return false;
    }
    const int mergeMode = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_MERGEFAST", 1))));
    const bool sameSerial = mergeMode != 0 && *c.lastRecSerial == st.serial;
    if (!(sameSerial || (p.texMask == st.texMask && SameStruct(p.rs, *st.rs) && SameWords(p.tex, st.tex, static_cast<uint32_t>(sizeof cur.tex / 4)))))
        return false;
    UniArena& uni = *c.uni;
    const size_t xo = c.blockOff[UB_MV];
    if (xo + 24 > uni.size() || (!conv && static_cast<size_t>(p.ublock[UB_MV]) + 24 > uni.size())) return false;
    if (!conv) {
        // (the record becomes copy 0 of the instance buffer, its u_mv slot 0 of a new palette)
        const size_t o = uni.size();
        uni.resize(o + static_cast<size_t>(K) * 8);
        float* pal = uni.data() + o;
        std::memset(pal, 0, static_cast<size_t>(K) * 8 * sizeof(float));
        const float* const l = uni.data() + p.ublock[UB_MV];
        std::memcpy(pal, l, 12 * sizeof(float));
        std::memcpy(pal + K * 4, l + 12, 12 * sizeof(float));
        p.vsId = j.vsTwin;
        p.ublock[UB_PAL] = static_cast<uint32_t>(o);
        p.vbuf = ib;
        p.vtxOffset = 0;
        p.vtxFirst = 0;
        p.idxOffset = 0;
        c.rig->rec = last;
        c.rig->slots = 1;
        c.rig->pal = static_cast<uint32_t>(o);
        if (!c.vtxLog) Bump(g_instRuns);
    }
    // (copy `slot` of the buffer is this draw's: its w names the slot; a slot per draw, even for a matrix equal to the one before)
    const uint32_t slot = c.rig->slots++;
    float* const pal = uni.data() + c.rig->pal;
    const float* const x = uni.data() + xo; // its u_mv block: position rows 0..2, normal rows 0..2 (4 floats each)
    std::memcpy(pal + slot * 12, x, 12 * sizeof(float));
    std::memcpy(pal + K * 4 + slot * 12, x + 12, 12 * sizeof(float));
    p.vtxCount += j.count;
    p.idxCount += j.idxCount;
    *c.lastRecSerial = st.serial;
    // (the record's pixels: the union, as FinishDraw's merges make it)
    if (p.ab) {
        if (!j.ab) p.ab = 0;
        else if (j.abW > 0 && j.abH > 0) {
            if (p.abW <= 0 || p.abH <= 0) { p.abX = j.abX; p.abY = j.abY; p.abW = j.abW; p.abH = j.abH; }
            else {
                const int ax = std::min<int>(j.abX, p.abX), ay = std::min<int>(j.abY, p.abY);
                const int bx = std::max<int>(j.abX + j.abW, p.abX + p.abW), by = std::max<int>(j.abY + j.abH, p.abY + p.abH);
                p.abX = static_cast<int16_t>(ax); p.abY = static_cast<int16_t>(ay);
                p.abW = static_cast<int16_t>(bx - ax); p.abH = static_cast<int16_t>(by - ay);
            }
        }
    }
    if (p.joined < 255) ++p.joined; // (ss24)
    if (!c.vtxLog) Bump(g_instJoins);
    return true;
}
// The palette twin of vertex shader `vsId` (RigidTwin's, from the registry's key: with GXV_DIRECT_J3D cur.vs may not hold the key now),
// 0xFFFF when the key cannot have one; memoised by id (ids name one key for the process's life).
std::vector<uint16_t> g_instTwin; // per vertex shader id: twin id + 1, 0xFFFF none possible, 0 not known yet
uint16_t StaticInstTwin(uint16_t vsId) {
    if (g_instTwin.size() <= vsId) g_instTwin.resize(static_cast<size_t>(vsId) + 64, 0);
    uint16_t m = g_instTwin[vsId];
    if (m == 0) {
        const VsKey k0 = g_vsReg.keys[vsId];
        bool ok = RigidMode() != 0 && k0.perVtxPos == 0;
        for (int i = 0; ok && i < k0.numTexGens; ++i) ok = !k0.tg[i].perVtxMtx;
        if (ok) {
            VsKey k = k0;
            k.perVtxPos = RigidMode() == 1 ? 2 : 3;
            const uint16_t t = Lookup(g_vsReg, k, false, GenerateVertexShader);
            m = t < 0xFFFE ? static_cast<uint16_t>(t + 1) : 0xFFFF;
        } else {
            m = 0xFFFF;
        }
        g_instTwin[vsId] = m;
    }
    return m == 0xFFFF ? 0xFFFF : static_cast<uint16_t>(m - 1);
}
} // namespace
void StaticInstCounters(uint64_t out[3]) {
    out[0] = g_instRuns.load(std::memory_order_relaxed);
    out[1] = g_instJoins.load(std::memory_order_relaxed);
    out[2] = g_instRefused.load(std::memory_order_relaxed);
}
// GXV_STATIC_INST (read once; default 0 = off): see "static instances" above. Needs GXV_RIGIDMERGE (its palette and palette twins); not
// with GXV_RIGIDMERGE_DLLEARN (whose record-stage tests read static jobs their own way). The slots a run of instances has: 10 with
// GXV_RIGIDMERGE=1, 4 with =2.
uint32_t StaticInstSlots() {
    const uint32_t k = static_cast<uint32_t>(VGX_KNOB([] () -> uint32_t {
        if (EnvInt("GXV_STATIC_INST", 0) <= 0) return 0u;
        const long mode = RigidMode();
        if (!mode || RigidMergeLearnMax() != 0) return 0u;
        return kRigidMaxSlotsOf[mode];
    }()));
    return k;
}
void RigidMergeCounters(uint64_t out[2]) { out[0] = g_rigRuns; out[1] = g_rigJoins; }
// GXV_RIGIDMERGE_DLMAX (read once, default 0, only with GXV_RIGIDMERGE on): a display list without matrix indices of up to this many vertices is not
// made a static buffer (vgx_dlcache.cpp): its vertices are copied into the frame's arena at each use, where its draw can join a palette run.
// (A static list's vertices are in a buffer of their own, and cannot be in a record with another list's.)
uint32_t RigidMergeListMax() {
    const uint32_t n = static_cast<uint32_t>(VGX_KNOB(RigidMode() ? static_cast<uint32_t>(EnvInt("GXV_RIGIDMERGE_DLMAX", 0)) : 0u));
    return n;
}
// GXV_RIGIDMERGE_DLMAX=learn, or GXV_RIGIDMERGE_DLLEARN=1 (read once, default off, only with GXV_RIGIDMERGE on): the cached lists of more than GXV_SOFTXFORM
// and up to N vertices (GXV_RIGIDMERGE_DLLEARN_MAX, default 512; GXV_RIGIDMERGE_DLLEARN=<n> with n >= 16 sets it too) are copied into the arena only where
// that has bought a palette run (vgx_dlcache.cpp). 0: off.
uint32_t RigidMergeLearnMax() {
    const uint32_t n = static_cast<uint32_t>(VGX_KNOB([] () -> uint32_t {
        if (!RigidMode()) return 0u;
        const char* dm = std::getenv("GXV_RIGIDMERGE_DLMAX");
        const long on = EnvInt("GXV_RIGIDMERGE_DLLEARN", 0);
        if (!(dm && std::strcmp(dm, "learn") == 0) && on == 0) return 0u;
        long cap = on >= 16 ? on : EnvInt("GXV_RIGIDMERGE_DLLEARN_MAX", 512);
        if (cap > 0xFFFF) cap = 0xFFFF;
        return cap > static_cast<long>(SoftXformLimit()) ? static_cast<uint32_t>(cap) : 0u;
    }()));
    return n;
}

// The record part of a draw: merged into the previous record when nothing but the geometry differs, else a new
// record; then the indices. (The front-end split runs this on its finishing thread: vgx_fesplit.h.)
uint32_t split::FinishDraw(FinishCtx& c, const DrawJob& j, const DrawStateRef& st) {
    // (GXV_NOMERGE=1 turns merging off: one record per primitive, for comparing runs.)
    std::vector<DrawRec>& draws = *c.draws;
    std::vector<uint16_t, NoInitAlloc<uint16_t>>& idx = *c.idx;
    DrawRec* rec = nullptr;
    uint32_t made = ~0u;
    const bool abs = AbsoluteIndices();
    const uint16_t chunk = static_cast<uint16_t>(j.chunk);
    uint32_t base = abs ? j.first : 0; // what the new primitive's 0-based indices are offset by
    const int last = *c.lastDraw;
    // (GXV_HUD_NATIVE: a 2D draw never joins a 3D record or the other way round; the flag is 0 for every draw with the switch off)
    const bool noMerge = VGX_KNOB(EnvInt("GXV_NOMERGE", 0) != 0) != 0 ||
                         (last >= 0 && last == static_cast<int>(draws.size()) - 1 && (draws[last].hud != ((j.cover >> 2) & 1u) || draws[last].owner != j.owner ||
                                                                                       draws[last].depthMark != j.depthMark || draws[last].playerMark != j.playerMark));
    const bool prevIsDraw = c.cmds ? (!c.cmds->empty() && c.cmds->back().type == CMD_DRAW) : j.prevIsDraw != 0;
    if (!noMerge && last >= 0 && last == static_cast<int>(draws.size()) - 1 && prevIsDraw) {
        DrawRec& p = draws[last];
        // (the new vertices follow the record's last one: by index within the same chunk, or by byte offset)
        const bool contiguous = abs ? (p.vchunk == chunk && p.vtxFirst + p.vtxCount == j.first)
                                    : (p.vtxOffset + static_cast<size_t>(p.vtxCount) * j.stride == j.vtxStart);
        // (the render state, textures and texture mask are the record's when they have not been built again since it was made:
        // GXV_MERGEFAST=0 compares them always, 2 compares them as well and logs a disagreement)
        const int mergeMode = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_MERGEFAST", 1))));
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
        } else if (j.vsTwin != 0xFFFF && RigidJoin<false>(c, p, last, j, st, [&] {
                       return p.vbuf == 0 && p.prim == j.prim && p.layoutMask == j.layoutMask && p.fsId == st.useFs && p.vtxCount + j.count <= 0xFFFF &&
                              contiguous && p.idxOffset + p.idxCount == idx.size() && (sameSerial || sameState());
                   })) {
            // (GXV_RIGIDMERGE: the draw differs from the record in its matrix only; it has a slot of the record's palette now)
            rec = &p;
            *c.lastRecSerial = st.serial;
            if (!abs) base = p.vtxCount;
        }
    }
    if (c.joinLog) c.joinLog->push_back(rec ? 1 : 0); // (GXV_RIGIDMERGE_DLLEARN)
    if (c.rig) c.rig->prevOk = j.vsTwin != 0xFFFF;
    if (workload::FeSplitOn()) {
        // (GXV_FE_SPLIT: why a draw did not join the record before it, the first test that failed)
        unsigned why = workload::MergeJoined;
        if (!rec) {
            why = workload::MergeOther;
            if (noMerge) why = workload::MergeOther;
            else if (!(last >= 0 && last == static_cast<int>(draws.size()) - 1 && prevIsDraw)) why = workload::MergeNoPrev;
            else {
                const DrawRec& p = draws[last];
                const bool contiguous = abs ? (p.vchunk == chunk && p.vtxFirst + p.vtxCount == j.first)
                                            : (p.vtxOffset + static_cast<size_t>(p.vtxCount) * j.stride == j.vtxStart);
                if (p.vbuf != 0) why = workload::MergeStatic;
                else if (p.prim != j.prim || p.layoutMask != j.layoutMask) why = workload::MergeLayout;
                else if (p.vsId != st.vsId || p.fsId != st.useFs) why = workload::MergeShader;
                else if (!contiguous || p.idxOffset + p.idxCount != idx.size()) why = workload::MergeVerts;
                else if (!SameWords(p.ublock, c.blockOff, UB_COUNT)) { why = workload::MergeBlocks; NoteBlockMiss(p.ublock, c.blockOff); }
                else if (!(p.texMask == st.texMask && SameWords(p.tex, st.tex, static_cast<uint32_t>(sizeof cur.tex / 4)))) why = workload::MergeTex;
                else if (!SameStruct(p.rs, *st.rs)) why = workload::MergeRs;
            }
        }
        workload::MergeCount[why].fetch_add(1, std::memory_order_relaxed);
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
        rec->cover = j.cover & 1u;
        rec->hud = (j.cover >> 2) & 1u;
        rec->owner = j.owner; // (GXV_HRW)
        rec->depthMark = j.depthMark; // (hrw41)
        rec->playerMark = j.playerMark; // (g30)
        rec->coverCull = (j.cover >> 1) & 1u; // (GXV_SHADOW_CULLCOVER: see the job's cover)
        rec->clearQ = static_cast<uint8_t>((j.cover >> 3) & 0x1Fu); rec->clearRgba = j.clearRgba; // (GXV_CLEARQ: ClearQMark; a record that grows by merging is refused by the back end: 4 vertices, 6 indices)
        rec->ab = j.ab;
        rec->abX = j.abX; rec->abY = j.abY; rec->abW = j.abW; rec->abH = j.abH;
    } else if (rec->ab) {
        // (GXV_SHADOWFILL_BOUNDS: a merged record's pixels are the union of its draws'; one draw not known, the record not known)
        if (!j.ab) rec->ab = 0;
        else if (j.abW > 0 && j.abH > 0) {
            if (rec->abW <= 0 || rec->abH <= 0) { rec->abX = j.abX; rec->abY = j.abY; rec->abW = j.abW; rec->abH = j.abH; }
            else {
                const int ax = std::min<int>(j.abX, rec->abX), ay = std::min<int>(j.abY, rec->abY);
                const int bx = std::max<int>(j.abX + j.abW, rec->abX + rec->abW), by = std::max<int>(j.abY + j.abH, rec->abY + rec->abH);
                rec->abX = static_cast<int16_t>(ax); rec->abY = static_cast<int16_t>(ay);
                rec->abW = static_cast<int16_t>(bx - ax); rec->abH = static_cast<int16_t>(by - ay);
            }
        }
    }
    if (made == ~0u && rec->joined < 255) ++rec->joined; // (ss24: merged into the record before it)
    if (made == ~0u && (j.exX != rec->exX || j.exY != rec->exY || j.exW != rec->exW || j.exH != rec->exH)) {
        // (merged: the record's bound is the union)
        const int ax = std::min<int>(j.exX, rec->exX), ay = std::min<int>(j.exY, rec->exY);
        const int bx = std::max<int>(j.exX + j.exW, rec->exX + rec->exW), by = std::max<int>(j.exY + j.exH, rec->exY + rec->exH);
        // (DrawRec::cover says the record's first primitive writes every pixel of its extent: true no longer when the extent grows;
        // two half-screen quads merged would otherwise claim the whole screen, and the depth plan's colour liveness - GXV_EFBSWAP's
        // drawing back - would take pixels the first quad never wrote as overwritten)
        if (ax != rec->exX || ay != rec->exY || bx - ax != rec->exW || by - ay != rec->exH) { rec->cover = rec->coverCull = 0; rec->depthMark &= 1u; }  // (halo31: nor is it one depth-only rectangle any more)
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

// ---- packed static runs: the matrices (GXV_VTXPACK; the runs themselves: vgx_dlcache.cpp "packed static runs")
// A packed run's attribute holds q = v * 2^k where the float buffer held v. Every read of that attribute in the vertex shader
// (vgx_shadergen.cpp GenerateVertexShader) is a dot product with matrix rows from a uniform block:
//   a_pos.xyz   eye = (u_mv[r] . (x, y, z, 1)), r = 0..2; or the palette's position rows (u_pal, perVtxPos 1 / 2 / 3)
//   a_nrm       n = (u_mv[3 + r].xyz . n); or the palette's normal rows
//   a texgen    tc = (u_tm[3i + r] . srcv) with srcv = (a_pos.xy, a_pos.z or 1, 1), (a_nrm.xy, a_nrm.z or 1, 1) or (a_texN, 1, 1)
// (a_pos.w, the matrix row, is not scaled: the run keeps it as it is). So the record gets copies of its u_mv / palette / texture matrix
// blocks with the columns that multiply a packed attribute times 2^-k: (m * 2^-k) * q and m * (q * 2^-k) are the same real number, so
// the GPU rounds both to the same float, and every sum after that is the same. The scaled copies are made here, in the record stage, in
// the arena the record's other blocks are in (as GXV_RIGIDMERGE's palettes are). What the scaling cannot follow, and so is never packed:
// a per-vertex texture matrix (palette rows that a position scale would also scale; runs with VL_TMI are not packed, and a key has
// perVtxMtx only with TEXnMTXIDX in its vertex descriptor) and the line / point quads (lpMode: dynamic draws only). VqTexOf marks such
// a key anyway (counted, "keys refused"; the record is drawn as it is). A matrix element that would become subnormal when scaled (a
// nonzero |m| < 2^(k-126)) is counted as "tiny": the one case where the picture might differ (from ~1e-30 on), expected never.
namespace {
std::atomic<uint64_t> g_vqRecords{0}, g_vqScaled{0}, g_vqTiny{0}, g_vqUnsafe{0};
inline void VqBump(std::atomic<uint64_t>& a, uint64_t v = 1) { a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); }
constexpr uint32_t kVqUnsafe = 0xFFFFFFFFu;
// How key k's texgens read packed attributes, 4 bits per texgen: 0 none (a colour texgen, a constant source, tex2..7), 1 a_pos.xy,
// 2 a_pos.xyz, 3 a_nrm.xy, 4 a_nrm.xyz, 5 a_tex0, 6 a_tex1. kVqUnsafe: the key reads one in a way the block scaling cannot follow.
uint32_t VqTexOf(const VsKey& k) {
    if (k.lpMode()) return kVqUnsafe;
    uint32_t v = 0;
    for (int i = 0; i < k.numTexGens && i < 8; ++i) {
        const VsKey::TexGen& t = k.tg[i];
        if (t.type >= 2) continue;
        if (t.perVtxMtx) return kVqUnsafe;
        uint32_t c = 0;
        if (t.src == 0) c = t.inputAbc1 ? 2 : 1;
        else if (t.src == 1) c = t.inputAbc1 ? 4 : 3;
        else if (t.src == 5) c = 5;
        else if (t.src == 6) c = 6;
        v |= c << (4 * i);
    }
    return v;
}
// x * 2^-k (s), noting a value the scaling cannot keep exact.
inline float VqMul(float x, float s, bool& tiny) {
    const float y = x * s;
    const int cx = std::fpclassify(x);
    if (cx == FP_SUBNORMAL || (cx == FP_NORMAL && std::fpclassify(y) != FP_NORMAL)) tiny = true;
    return y;
}
// A copy of block `b` of the record (n floats) at the arena's end, the record pointed at it; null when the record has none.
float* VqCopyBlock(UniArena& uni, DrawRec& rec, int b, size_t n) {
    const uint32_t o = rec.ublock[b];
    if (o == kNoBlock || static_cast<size_t>(o) + n > uni.size()) return nullptr;
    const size_t at = uni.size();
    uni.resize(at + n);
    std::memcpy(uni.data() + at, uni.data() + o, n * sizeof(float));
    rec.ublock[b] = static_cast<uint32_t>(at);
    return uni.data() + at;
}
void VqScaleBlocks(split::FinishCtx& c, DrawRec& rec, const split::DrawJob& j) {
    if (!c.uni) return;
    const bool unsafe = j.vqTex == kVqUnsafe; // (counted by EmitStaticRun; its positions and normals are still scaled)
    UniArena& uni = *c.uni;
    const int kp = static_cast<int>(j.vqK & 31u), kn = static_cast<int>((j.vqK >> 5) & 31u);
    const int kt[2] = {static_cast<int>((j.vqK >> 10) & 31u), static_cast<int>((j.vqK >> 15) & 31u)};
    const float sp = std::ldexp(1.0f, -kp), sn = std::ldexp(1.0f, -kn), st[2] = {std::ldexp(1.0f, -kt[0]), std::ldexp(1.0f, -kt[1])};
    bool tiny = false, missing = false;
    uint32_t scaled = 0;
    auto rows = [&](float* blk, int first, int count, float s, int cols) {
        for (int r = first; r < first + count; ++r)
            for (int q = 0; q < cols; ++q) blk[r * 4 + q] = VqMul(blk[r * 4 + q], s, tiny);
    };
    if (kp || kn) {
        if (j.vqPal == 0) {
            if (float* mv = VqCopyBlock(uni, rec, UB_MV, 24)) { // (rows 0..2 position, 3..5 normal)
                if (kp) rows(mv, 0, 3, sp, 3);
                if (kn) rows(mv, 3, 3, sn, 3);
                ++scaled;
            } else {
                missing = true;
            }
        } else {
            // (the palette as the key declares it: full 64 position rows + 32 normal rows; short / small K + K)
            const int K = j.vqPal == 1 ? 64 : PalRowsOfKey(j.vqPal), N = j.vqPal == 1 ? 32 : K;
            if (float* pal = VqCopyBlock(uni, rec, UB_PAL, static_cast<size_t>(K + N) * 4)) {
                if (kp) rows(pal, 0, K, sp, 3);
                if (kn) rows(pal, K, N, sn, 3);
                ++scaled;
            } else {
                missing = true;
            }
        }
    }
    // texture matrices: the columns that multiply a packed source (x, y; z for the (a, b, c, 1) input form)
    bool tex = false;
    for (int i = 0; i < 8 && !tex && !unsafe; ++i) {
        const uint32_t cl = (j.vqTex >> (4 * i)) & 15u;
        tex = ((cl == 1 || cl == 2) && kp) || ((cl == 3 || cl == 4) && kn) || (cl == 5 && kt[0]) || (cl == 6 && kt[1]);
    }
    if (tex) {
        // (texture rows 3 per texgen, then from float 96 the post rows, copied untouched: what the shader reads of the block)
        if (float* tm = VqCopyBlock(uni, rec, UB_TEXM, j.vqTmLen)) {
            for (int i = 0; i < 8 && 12 * i + 12 <= j.vqTmLen && i * 12 < 96; ++i) {
                const uint32_t cl = (j.vqTex >> (4 * i)) & 15u;
                const float s = (cl == 1 || cl == 2) ? sp : (cl == 3 || cl == 4) ? sn : cl == 5 ? st[0] : cl == 6 ? st[1] : 1.0f;
                if (s != 1.0f) rows(tm, 3 * i, 3, s, (cl == 2 || cl == 4) ? 3 : 2);
            }
            ++scaled;
        } else {
            missing = true;
        }
    }
    if (!c.vtxLog) {
        VqBump(g_vqScaled, scaled);
        if (tiny) VqBump(g_vqTiny);
        if (missing) VqBump(g_vqUnsafe); // (a block the scaling needed was not there: counted with the refused keys, expected never)
    }
}
} // namespace
void VtxPackDrawCounters(uint64_t out[4]) {
    out[0] = g_vqRecords.load(std::memory_order_relaxed);
    out[1] = g_vqScaled.load(std::memory_order_relaxed);
    out[2] = g_vqTiny.load(std::memory_order_relaxed);
    out[3] = g_vqUnsafe.load(std::memory_order_relaxed);
}

uint32_t split::FinishStatic(FinishCtx& c, const DrawJob& j, const DrawStateRef& st) {
    if (c.joinLog) {
        // GXV_RIGIDMERGE_DLLEARN: would the draw join the previous record if its list were copied into the arena (and the previous draw's too, if it is
        // a static one)? The tests are FinishDraw's, but for the vertices' places.
        bool would = false;
        const int last = *c.lastDraw;
        const bool prevIsDraw = c.cmds ? (!c.cmds->empty() && c.cmds->back().type == CMD_DRAW) : j.prevIsDraw != 0;
        std::vector<DrawRec>& draws = *c.draws;
        if (j.copyable && j.vsTwin != 0xFFFF && c.rig && c.rig->prevOk && last >= 0 && last == static_cast<int>(draws.size()) - 1 && prevIsDraw) {
            DrawRec& p = draws[last];
            const int mergeMode = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_MERGEFAST", 1))));
            const bool sameSerial = mergeMode != 0 && *c.lastRecSerial == st.serial;
            would = RigidJoin<true>(c, p, last, j, st, [&] {
                return p.prim == j.prim && p.layoutMask == j.layoutMask && p.fsId == st.useFs && p.playerMark == j.playerMark && p.vtxCount + j.count <= 0xFFFF &&
                       (sameSerial || (p.texMask == st.texMask && SameStruct(p.rs, *st.rs) && SameWords(p.tex, st.tex, static_cast<uint32_t>(sizeof cur.tex / 4))));
            });
        }
        c.joinLog->push_back(would ? 1 : 0);
        if (c.rig) c.rig->prevOk = j.copyable && j.vsTwin != 0xFFFF;
    } else if (j.chunk != 0 && StaticInstJoin(c, j, st)) {
        return ~0u; // (GXV_STATIC_INST: joined the record before it, a run of instances)
    }
    DrawRec* rec = NewDrawRec(c, j.prim, j.layoutMask, st);
    rec->hud = (j.cover >> 2) & 1u; // (GXV_HUD_NATIVE)
    rec->owner = j.owner; // (GXV_HRW)
    rec->depthMark = j.depthMark; // (hrw41)
    rec->playerMark = j.playerMark; // (g30)
    rec->vbuf = j.vbuf;
    rec->vtxOffset = j.vtxOffset;
    rec->vtxFirst = j.vtxFirst;
    rec->idxOffset = j.idxOffset;
    rec->vtxCount = j.count;
    rec->idxCount = j.idxCount;
    rec->ab = j.ab; // (GXV_SHADOWFILL_BOUNDS: the list's bounds through the draw's matrices)
    rec->abX = j.abX; rec->abY = j.abY; rec->abW = j.abW; rec->abH = j.abH;
    if (j.layoutMask & VL_QALL) { // (GXV_VTXPACK: a packed run; its matrices take the shifts)
        if (!c.vtxLog) VqBump(g_vqRecords);
        if (j.vqK) VqScaleBlocks(c, *rec, j);
    }
    return static_cast<uint32_t>(c.draws->size() - 1);
}

static std::vector<uint8_t> g_joinLogMain; // GXV_RIGIDMERGE_DLLEARN: the single-thread path's join log of the frame
static uint32_t g_jobSeq = 0, g_lastJobKey = 0; // jobs of this frame so far; the shader and state key of the last one (GXV_RIGIDMERGE_DLLEARN)
static uint32_t g_hashedSerial = ~0u, g_stateHash = 0;
uint32_t DlJobSeq() { return g_jobSeq; }
uint32_t DlLastJobKey() { return g_lastJobKey; }
static bool g_pendingCopyable = false;
void SetPendingStaticCopyable(bool on) { g_pendingCopyable = on; }
static uint32_t g_pendingInst = 0; // (GXV_STATIC_INST: the instance buffer of the list whose runs come next; vgx_dlcache.cpp Emit)
void SetPendingStaticInst(uint32_t bufId) { g_pendingInst = bufId; }
static inline split::FinishCtx MainFinishCtx() {
    return split::FinishCtx{&g_buf.draws, &g_buf.idx, &g_buf.cmds, g_blockOff, &g_lastDraw, &g_lastRecSerial, &stats::Cur().drawCalls, true, &g_buf.uni, &g_rig, nullptr,
                              RigidMergeLearnMax() ? &g_joinLogMain : nullptr};
}
static inline split::DrawStateRef CurStateRef() { return split::DrawStateRef{cur.vsId, cur.useFs, cur.texMask, cur.stateSerial, &cur.rs, cur.tex}; }

// ==================================================================== prewarm24: the warm-up scope's variants (GXV_PREWARM)
// What a draw inside the warm-up scope leaves: the program variant it would be recorded with (the vertex shader, the fragment shader the
// record takes - Resolved::useFs -, the layout and the blend key: what the back end's NoteVariant / StageNote see for the record), queued
// for the back end as a CMD_VARIANT (in the frame's deferred warm list when the host does the warm-up in slices: RunWarm, never in a frame
// in play), and nothing else: no record, no vertices, no uniform block. The warm-up list file learns it as a draw's NoteVariant would.
// Each variant goes once per stage (the back end's own dedup leaves an already linked program alone; a program evicted since is linked again).
// The flag bit on the command's argument tells the back end the variant is the warm-up scope's (its own counts and the hold); variants indices
// are far below it.
constexpr uint32_t kPrewarmVarBit = 0x80000000u;
struct PrewarmCtr {
    uint64_t scopes = 0, draws = 0, derived = 0, queued = 0, alt = 0, leaked = 0, unmatched = 0, stageKey = 0;
} g_pw;
std::unordered_set<uint64_t> g_pwSent; // VariantKey of the variants queued for the stage g_pw.stageKey
extern "C" uint32_t port_scene_key(void) __attribute__((weak));
// precomp26 (GXV_STAGE_ENUM): the variant of a dry draw, as the stage lists hold a record (the two shader keys' hashes, layout mask << 13 | blend key), for the back end's
// inbox; each once per stage. Not to the back end's warm-up queue (those variants are linked at once) and not to the shader list file (the list is what draws made).
senum::Inbox& StageEnumInbox() { static senum::Inbox in; return in; }
namespace {
struct EnumCtr { uint64_t markers = 0, outsideScope = 0, noPlan = 0, draws = 0, variants = 0, dup = 0, small = 0; uint32_t stage = 0xFFFFFFFFu, seq = 0; } g_enc;
std::unordered_set<uint64_t> g_enSent; // senum::RecKey of the records pushed for g_enc.stage
}
void EnumNote(uint16_t vsId, uint16_t fsId, uint32_t layoutMask, uint32_t blendKey) {
    if (vsId >= g_vsReg.sources.size() || fsId >= g_fsReg.sources.size()) return;
    const uint32_t stage = port_scene_key ? port_scene_key() : 0;
    if (stage != g_enc.stage) { g_enc.stage = stage; g_enSent.clear(); g_enc.seq = 0; }
    senum::Rec r;
    r.vs = g_vsReg.sources[vsId]->keyHash;
    r.fs = g_fsReg.sources[fsId]->keyHash;
    r.lb = ((layoutMask & 0xFFFu) << 13) | (blendKey & 0x1FFFu);
    r.stage = stage;
    r.seq = g_enc.seq;
    if (!g_enSent.insert(senum::RecKey(r)).second) { ++g_enc.dup; return; }
    ++g_enc.variants;
    StageEnumInbox().Push(r);
}
void WarmNote(uint16_t vsId, uint16_t fsId, uint32_t layoutMask, uint32_t blendKey) {
    if (__builtin_expect(g_enumDry, 0)) { EnumNote(vsId, fsId, layoutMask, blendKey); return; } // (precomp26)
    ++g_pw.derived;
    const uint32_t stage = port_scene_key ? port_scene_key() : 0;
    if (stage != g_pw.stageKey) { g_pw.stageKey = stage; g_pwSent.clear(); }
    const uint64_t vk = VariantKey(vsId, fsId, layoutMask, blendKey);
    if (!g_pwSent.insert(vk).second) return;
    ++g_pw.queued;
    if (g_shaderList) { // (the warm-up list learns it, as split::NoteVariant does for a draw)
#if GXV_RECIPE_SEED_BATCH
        if (g_recipeMerged) PersistRecipeDraw(vsId, fsId, layoutMask & 0xFFFu, blendKey);
        else {
#endif
        const uint32_t pk = (static_cast<uint32_t>(vsId) << 16) | fsId;
        two::AGuard lockedForTheSecondWorker;
        if (g_progSeen.insert(pk).second) {
            const uint32_t v[2] = {vsId, fsId};
            ListAppend(3u, v, sizeof v);
        }
        if (g_varSeen.insert(vk).second) {
            const uint32_t v[4] = {vsId, fsId, layoutMask & 0xFFFu, blendKey};
            ListAppend(4u, v, sizeof v);
        }
#if GXV_RECIPE_SEED_BATCH
        }
#endif
    }
    g_buf.variants.push_back(VariantCmd{vsId, fsId, static_cast<uint16_t>(layoutMask & 0xFFFu), static_cast<uint16_t>(blendKey & 0x1FFFu)});
    (g_warmDeferred ? g_buf.warm : g_buf.cmds).push_back(Cmd{CMD_VARIANT, static_cast<uint32_t>(g_buf.variants.size() - 1) | kPrewarmVarBit});
}
// (tests and the back end's log line) [0] scopes, [1] draws, [2] variants derived, [3] queued (new to the stage), [4] unfolded twins of folded
// draws, [5] draws that reached the record path inside a scope by another way (taken there, not drawn), [6] markers without their partner
void PrewarmCounters(uint64_t out[7]) {
    out[0] = g_pw.scopes; out[1] = g_pw.draws; out[2] = g_pw.derived; out[3] = g_pw.queued; out[4] = g_pw.alt; out[5] = g_pw.leaked; out[6] = g_pw.unmatched;
}

// The record part of a draw: here, or (front-end split) a placeholder command and a job for the finishing thread.
static inline void FinishOrSend(const split::DrawJob& j, const split::DrawStateRef* over = nullptr) {
    const split::DrawStateRef st = over ? *over : CurStateRef();
    if (__builtin_expect(g_warmScope, 0)) {
        // (prewarm24: a draw that came here by a way EmitDraw / EmitStaticDraw do not take inside the scope - a templated J3D run -: its
        // variant, and it is not drawn; a dynamic draw's vertices stay in the arena unused)
        ++g_pw.leaked;
        WarmNote(st.vsId, st.useFs, j.layoutMask, BlendKey(*st.rs));
        return;
    }
    if (__builtin_expect(g_lwCensus, 0)) LwCensusDraw(st.vsId, st.useFs, j.layoutMask, BlendKey(*st.rs)); // (layoutwarm24: a layout's draw in play, counted)
    ++g_jobSeq;
    if (RigidMergeLearnMax()) {
        // (GXV_RIGIDMERGE_DLLEARN: what a learned list's use is told apart by: the shaders and the render state, textures and texture mask of the draw before it.
        // The state's serial says when to hash it again.)
        if (st.serial != g_hashedSerial || over) {
            g_hashedSerial = over ? ~0u : st.serial;
            uint64_t h = XXH3_64bits(st.rs, sizeof(RenderState));
            h = XXH3_64bits_withSeed(st.tex, sizeof cur.tex, h);
            g_stateHash = static_cast<uint32_t>(h ^ (h >> 32)) + st.texMask;
        }
        g_lastJobKey = (g_stateHash * 0x9E3779B1u) ^ ((static_cast<uint32_t>(st.vsId) << 16) | st.useFs);
    }
    if (j.vsTwin != 0xFFFF && (j.kind == split::JOB_DYNAMIC || (j.kind == split::JOB_STATIC && j.chunk != 0))) { // (GXV_STATIC_INST: an instance job too)
        // (GXV_RIGIDMERGE: the palette twin of the key may be what a record is drawn with: its variant goes on the warm-up list too)
        static struct { uint32_t ids = ~0u, layout = 0, serial = 0; } lastTwin;
        const uint32_t ids = (static_cast<uint32_t>(j.vsTwin) << 16) | st.useFs;
        if (ids != lastTwin.ids || j.layoutMask != lastTwin.layout || st.serial != lastTwin.serial) {
            lastTwin.ids = ids;
            lastTwin.layout = j.layoutMask;
            lastTwin.serial = st.serial;
            split::DrawStateRef t = st;
            t.vsId = j.vsTwin;
            split::NoteVariant(t, j.layoutMask);
        }
    }
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

// ================================================================ GX lines and points as quads (GXV_FIX_LINES)
// GX rasterises a line as a parallelogram widened by width/6 EFB pixels along the minor axis of its screen-space direction, and a point as a square of
// size/6 pixels (vgx_fixes.h). vitaGL ignores glLineWidth for custom shaders and the generated vertex shader writes no point size, so with GXV_FIX_LINES=1
// an immediate line or point primitive is widened into quads here and in the vertex shader (VsKey::lpMode, vgx_shadergen.cpp): every segment becomes four
// vertices (each end point twice, its own attributes), which carry in three free texture-coordinate slots the other end point's position and the corner's
// NDC offset (+-width / viewport size), or for a point in one slot the corner. The draw is then an ordinary quad list, drawn without culling (GX does not
// cull lines). Lines and points of a display list the cache keeps as a static buffer (EmitStaticDraw / EmitStaticRun) are not touched.
static bool LinesFix() {
    const bool on = VGX_KNOB(EnvInt("GXV_FIX_LINES", 0) != 0) != 0; // read once; default 0 (off): lines are 1 pixel, points of an undefined size, as before
    return on;
}
struct LpResult {
    uint16_t vsId = 0;
    uint32_t mask = 0, count = 0;
};
// 0: not expanded (the pending vertices are as they were), 1: expanded (the pending vertices are the quads, `r` says how), 2: the arena refused the vertices
// (the draw is lost).
static int ExpandLinePoints(uint8_t gxPrim, uint32_t layoutMask, uint32_t count, LpResult& r) {
    const uint32_t kind = gxPrim & 0xF8u;
    const bool pts = kind == 0xB8u;
    const uint32_t nSeg = pts ? count : (kind == 0xA8u ? count / 2 : (count >= 2 ? count - 1 : 0));
    if (nSeg == 0 || nSeg * 4u > 0xFFFFu) return 0;
    const RenderState& rs = cur.rs;
    if (!(rs.vpW > 0.0f && rs.vpH > 0.0f)) return 0;
    const uint32_t lps = g.bp[0x22];
    const uint32_t w6 = fixes::LinePointSize6(lps, pts);
    if (w6 == 0) return 0;
    uint32_t used = (layoutMask >> 3) & 0xFFu; // (the tex slots the layout has, then the ones a texgen of the key reads)
    for (int i = 0; i < cur.vs.numTexGens; ++i) {
        const VsKey::TexGen& t = cur.vs.tg[i];
        if (t.type < 2 && t.src >= 5 && t.src <= 12) used |= 1u << (t.src - 5);
    }
    const int need = pts ? 1 : 3;
    const int base = fixes::LinePointFreeSlots(used, need);
    if (base < 0) return 0;
    uint32_t code = fixes::LinePointTexCode(lps, pts), texMask = 0;
    if (code)
        for (int i = 0; i < cur.vs.numTexGens; ++i)
            if (fixes::LinePointTexEnabled(g.bp[0x30 + 2 * i], pts)) texMask |= 1u << i;
    if (!texMask) code = 0;
    VsKey k = cur.vs;
    k.setLp(pts ? 2 : 1, static_cast<uint8_t>(base), static_cast<uint8_t>(code), static_cast<uint8_t>(texMask));
    r.vsId = Lookup(g_vsReg, k, false, GenerateVertexShader);
    r.mask = layoutMask | (((1u << need) - 1u) << (3 + base));
    r.count = nSeg * 4u;

    const VtxLayout oL = LayoutOf(layoutMask);
    const VtxLayout nL = LayoutOf(r.mask);
    static std::vector<uint8_t> tmp; // (the player thread only)
    tmp.assign(g_buf.vtx.data() + g_pendingVtxStart, g_buf.vtx.data() + g_pendingVtxStart + static_cast<size_t>(count) * oL.stride);
    RestoreBvSnapshot();
    uint8_t* out = BeginVertices(r.mask, r.count);
    if (!out) {
        out = BeginVertices(layoutMask, count);
        if (!out) return 2;
        std::memcpy(out, tmp.data(), tmp.size());
        return 0;
    }
    const float px = static_cast<float>(w6) / 6.0f;
    const float ox = px / rs.vpW, oy = px / rs.vpH;
    const uint32_t nS = oL.stride, nT = nL.stride;
    const uint8_t* sv = tmp.data();
    const auto copyVtx = [&](uint8_t* d, const uint8_t* v) {
        std::memcpy(d, v, 16);
        if (layoutMask & VL_NRM) std::memcpy(d + nL.offNrm, v + oL.offNrm, 12);
        if (layoutMask & VL_COL0) std::memcpy(d + nL.offCol0, v + oL.offCol0, 4);
        if (layoutMask & VL_COL1) std::memcpy(d + nL.offCol1, v + oL.offCol1, 4);
        for (int t = 0; t < 8; ++t)
            if (layoutMask & (VL_TEX0 << t)) std::memcpy(d + nL.offTex[t], v + oL.offTex[t], 8);
        if (layoutMask & VL_TMI) std::memcpy(d + nL.offTmi, v + oL.offTmi, 8);
    };
    static const int8_t kPt[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}; // (left/right, top/bottom: GL y is down the picture, see BuildProjC)
    for (uint32_t s = 0; s < nSeg; ++s) {
        const uint32_t a = pts ? s : (kind == 0xA8u ? 2 * s : s), b = pts ? s : a + 1;
        for (int c = 0; c < 4; ++c) {
            uint8_t* d = out + static_cast<size_t>(4 * s + c) * nT;
            const uint32_t own = c < 2 ? a : b, other = c < 2 ? b : a;
            copyVtx(d, sv + static_cast<size_t>(own) * nS);
            float lc[2];
            if (pts) {
                lc[0] = kPt[c][0] * ox;
                lc[1] = kPt[c][1] * oy;
                std::memcpy(d + nL.offTex[base], lc, 8);
            } else {
                const float side = (c == 0 || c == 3) ? -1.0f : 1.0f;
                lc[0] = side * ox;
                lc[1] = side * oy;
                std::memcpy(d + nL.offTex[base], sv + static_cast<size_t>(other) * nS, 16);
                std::memcpy(d + nL.offTex[base + 2], lc, 8);
            }
        }
    }
    VGX_LOG_N(12, "lines: %s widened to quads: %u segment(s), size %u/6 px, tex offset code %u, mask %03X\n", pts ? "points" : (kind == 0xA8u ? "GX_LINES" : "GX_LINESTRIP"), nSeg, w6, code, r.mask);
    return 1;
}

// GXV_TEXBAKE (see TexBakeMask): the matrices of the texgens in `mask` into the texture coordinates of the pending vertices, from
// the registers as they are now (the block of the draw holds identity rows for them). False, with nothing changed, when the
// layout lacks an attribute a baked texgen reads (the caller then draws the primitive with the vertex shader's transform).
uint64_t g_texBakeDraws = 0, g_texBakeNew = 0, g_texBakeIdent = 0, g_texBakeUnsoft = 0; // (census: GXV_TEXBAKE=1 logs it now and then)
float g_texBakeLast[8][8];                                                              // (the rows the draw before baked, per texgen)
static bool BakeTexCoords(uint8_t* v0, uint32_t count, const VtxLayout& L, uint32_t mask) {
    for (uint32_t m = mask; m; m &= m - 1)
        if (!(L.mask & (VL_TEX0 << (cur.vs.tg[__builtin_ctz(m)].src - 5)))) return false;
    bool changed = false, ident = true;
    for (uint32_t m = mask; m; m &= m - 1) {
        const int i = __builtin_ctz(m);
        const uint32_t ti = TexGenRow(g, i);
        const float* r0 = g.xfPos[ti < 64 ? ti : 63];
        const float* r1 = g.xfPos[ti + 1 < 64 ? ti + 1 : 63];
        if (!SameWords(g_texBakeLast[i], r0, 4) || !SameWords(g_texBakeLast[i] + 4, r1, 4)) {
            changed = true;
            CopyWords(g_texBakeLast[i], r0, 4);
            CopyWords(g_texBakeLast[i] + 4, r1, 4);
        }
        // (the identity rows: the shader's dot product gives s and t as they are; nothing to do)
        static const float kId[8] = {1, 0, 0, 0, 0, 1, 0, 0};
        if (SameWords(r0, kId, 4) && SameWords(r1, kId, 4)) continue;
        ident = false;
        const float a0 = r0[0], a1 = r0[1], a2 = r0[2], a3 = r0[3], b0 = r1[0], b1 = r1[1], b2 = r1[2], b3 = r1[3];
        uint8_t* p = v0 + L.offTex[cur.vs.tg[i].src - 5];
        for (uint32_t k = 0; k < count; ++k, p += L.stride) {
            float st[2];
            std::memcpy(st, p, 8);
            // dot(row, vec4(s, t, 1, 1)) as the vertex shader has it: summed in pairs, (x0 y0 + x2 y2) + (x1 y1 + x3 y3), the order
            // Mesa's llvmpipe uses (host test: bit for bit with arbitrary matrices; left to right differed in 1 pixel of 12,288 by 1).
            // The console's GPU may sum otherwise: then a coordinate differs in its last bit at most.
            const float s = (a0 * st[0] + a2) + (a1 * st[1] + a3);
            const float t = (b0 * st[0] + b2) + (b1 * st[1] + b3);
            st[0] = s;
            st[1] = t;
            std::memcpy(p, st, 8);
        }
    }
    ++g_texBakeDraws;
    g_texBakeNew += changed;
    g_texBakeIdent += ident;
    if (diag::kEnabled && (g_texBakeDraws & 0x3FFFu) == 0)
        VGX_LOG_N(400, "texbake: %llu CPU-transformed draws had their texture matrix baked into the coordinates; %llu of them with matrix rows "
                       "other than the draw before (a block and a draw call of their own without GXV_TEXBAKE), %llu identity; %llu drawn with the "
                       "vertex shader's transform instead (an attribute missing)\n",
                  static_cast<unsigned long long>(g_texBakeDraws), static_cast<unsigned long long>(g_texBakeNew),
                  static_cast<unsigned long long>(g_texBakeIdent), static_cast<unsigned long long>(g_texBakeUnsoft));
    return true;
}
void TexBakeCounters(uint64_t out[4]) { out[0] = g_texBakeDraws; out[1] = g_texBakeNew; out[2] = g_texBakeIdent; out[3] = g_texBakeUnsoft; }

// ================================================================ shadow fill bounds (GXV_SHADOWFILL_BOUNDS), part 1: where alpha is written
// Every game picture copies the EFB's alpha to a texture (ScreenAlphaCapture, GX_CTF_A8) and draws a screen quad that reads it
// (MR::fillSilhouetteColor): a whole-EFB copy (the EFB stored, a copy scene, the EFB loaded again) and a full-screen alpha-tested
// blended pass, whether or not any shadow volume or silhouette put alpha anywhere. Part 2 (ShadowFillPass, at the frame's end)
// follows which EFB pixels may hold an alpha other than 0. For that it needs, for every draw that writes alpha other than the
// constant 0, the pixels it can write: worked out here, when the draw is made, from its vertices (or its display list's bounds)
// through the matrices and the projection of the moment (the very numbers BuildMtxC / BuildProjC put into u_mv, u_pal and u_proj),
// kept in DrawJob / DrawRec::ab. Only for those draws (shadow volumes, the player's silhouette, a few others), so the cost is small.
// Conservative: a vertex not safely in front of the eye (clip w not above 8 times its rounding bound), a value out of range, a
// line or point, a matrix row the shader would read elsewhere: not known (the scissor is all that is known).
// GXV_SHADOW_RECT (shadowpx23, "shadow rectangle" at ShadowFillPass): 0 off, 1 on, 2 worked out and logged only. Read once.
int ShadowRectMode() {
    const int m = static_cast<int>(VGX_KNOB(std::clamp<long>(EnvInt("GXV_SHADOW_RECT", 0), 0, 2)));
    return m;
}
// GXV_SHADOW_RECT_FE (shadowfe24; read once, default 0 = the bookkeeping below as it was): 1 = the same alpha bounds for less front-end work
// (exact: the same numbers, so the same command buffers; see "shadow fill bounds, part 1"), 2 = both ways, compared per draw (host tests).
// fix47: with GXV_CORNERPASS on and this switch at 0 the cheap way (1) is used: the corner pass needs the bounds on the console, where GXV_SHADOWFILL_BOUNDS and
// GXV_SHADOW_RECT are off because the old way's front-end time cost more than they saved (entry.cpp, 10 Nov pairs). =2 (compare) stays 2.
int ShadowRectFeMode() {
    const int m = static_cast<int>(VGX_KNOB([] {
        const int e = static_cast<int>(std::clamp<long>(EnvInt("GXV_SHADOW_RECT_FE", 0), 0, 2));
        return e == 0 && EnvInt("GXV_CORNERPASS", 0) != 0 ? 1 : e;
    }()));
    return m;
}
// The alpha bounds are worked out for GXV_SHADOWFILL_BOUNDS and for GXV_SHADOW_RECT (which needs the same set N).
bool ShadowFillOn() {
    const bool on = PORT_STAGE_KNOB(int32_t, port::stageknob::Int("GXV_SHADOWFILL_BOUNDS", 1) != 0 || ShadowRectMode() != 0) != 0;
    return on;
}
// fix47 (22 Nov): the alpha bounds and the set N are worked out for GXV_CORNERPASS too. imp45 put the corner's bookkeeping inside ShadowFillPass, which returns at
// once when ShadowFillOn() is false - and it is false on the console (entry.cpp: GXV_SHADOWFILL_BOUNDS=0 since r95, GXV_SHADOW_RECT=0): the corner copy was never
// even looked at ("cornerpass: ... small alpha copies seen 0", c35t3 Beach Bowl, =1 and =2). With only GXV_CORNERPASS on, N is worked out and nothing else changes:
// no fill is cut or left out, no capture made a fill (ShadowFillCuts() stays false), no "shadowfill:" line. Not a reason to refuse GXV_J3DNATIVE (j3n2::DrawUsable):
// a draw with no bounds counts for its whole scissor, which can only keep the corner copy where it is.
// cp50 (22 Nov): the corner pass pays for its bookkeeping only while it can engage. The back end (PlanCornerPass) switches it off for the rest of the stage when the marked
// copies of 240 pictures were all left alone (planned 0: Honeyhive's copies were taken by GXV_HOIST_GRID's own first copy already, ~1 ms a drawn turn of alpha bounds for
// nothing) and on again at the next stage load. 1 = off by that rule. Atomic: the back end's thread sets it, the front end reads it.
std::atomic<int> g_cpAutoOff{0};
// The corner rectangle (+ margin) of the last picture's copy, mirrored from CornerTrack for the draw-time test below.
struct CpRm { EfbRect rm{}; bool have = false; } g_cpRm;
inline bool CornerPassActive() { return CornerPassMode() != 0 && g_cpAutoOff.load(std::memory_order_relaxed) == 0; }
bool SfBoundsOn() {
    return ShadowFillOn() || CornerPassActive();
}
// cp50: the bounds of THIS draw are worth working out. With GXV_SHADOWFILL_BOUNDS / GXV_SHADOW_RECT on: every draw as before. With the corner pass alone N is only ever read
// over the corner rectangle, so only a draw whose scissor reaches it needs bounds; any other draw cannot write there whatever its bounds are (its scissor is a superset
// of its pixels), and the pass takes its extent cut to its scissor for it. Before the rectangle is known (the first picture) nothing can be marked: no bounds either.
// ss24 (GXV_SS_TILEBOUND): a draw that can be a back-face shadow volume (alpha added: blend ONE / ONE, ADD, alpha written, no destination-alpha zeroing) needs its pixel bounds for the stencil
// pass's tile grid. Only those draws (SsClassify's class 1 has the same state), only while the stencil pass runs.
inline int SsTileMode() { // GXV_SS_TILEBOUND: 0 off (c51), 1 on, 2 counted only (read once; the pass's description is at SsTileCtr)
    static const int m = static_cast<int>(std::clamp<long>(EnvInt("GXV_SS_TILEBOUND", 0), 0, 2));
    return m;
}
inline bool SsTileWanted() {
    if (SsTileMode() == 0 || ShadowStencilMode() < 1) return false;
    const RenderState& rs = cur.rs;
    return rs.alphaWrite && rs.blend && rs.blendSrc == BF_ONE && rs.blendDst == BF_ONE && rs.blendEq == BE_ADD && rs.dstAlphaZero == 0;
}
inline bool SfBoundsWanted() {
    if (ShadowFillOn()) return true;
    if (SsTileWanted()) return true;
    if (!CornerPassActive() || !g_cpRm.have) return false;
    const RenderState& rs = cur.rs;
    const EfbRect& r = g_cpRm.rm; // (Overlap is defined further down)
    return rs.scX < r.x + r.w && r.x < rs.scX + rs.scW && rs.scY < r.y + r.h && r.y < rs.scY + rs.scH && rs.scW > 0 && rs.scH > 0;
}
// GXV_SHADOW_CULLCOVER (gpuab23; default 1, only while GXV_SHADOWFILL_BOUNDS or GXV_SHADOW_RECT is on): the alpha bookkeeping below (N,
// "shadow fill bounds") takes a full rectangle drawn under a cull mode that keeps it as covering its pixels, as it takes one drawn
// without culling. Why: the game runs three XFBs (JUTXfb, MainLoopFramework::exchangeXfb_triple / copyXfb_triple), so its display copy
// does NOT clear (GXCopyDisp(.., GX_FALSE)); the EFB is cleared at the start of each picture by MainLoopFramework::clearEfb, a screen
// quad with GXSetDstAlpha(TRUE, 0), no blending, z ALWAYS - and GX_CULL_BACK. With only cull-NONE quads taken, nothing ever took the
// screen out of N: on the console (gpuwf-c21, 703/704) every alpha capture was marked "100.0% of the EFB" and every silhouette fill
// stayed "full". 0: as before (cull-NONE quads only).
bool ShadowCullCover() {
    const bool on = VGX_KNOB(EnvInt("GXV_SHADOW_CULLCOVER", 1) != 0 && SfBoundsOn()) != 0;
    return on;
}
// GXV_FIRSTCLEAR (gpucut23, vgx_backend_gl.cpp PlanFrame): the culled covering rectangle is marked (DrawRec::coverCull) for the depth plan too.
bool FirstClearMark() {
    const bool on = VGX_KNOB(EnvInt("GXV_FIRSTCLEAR", 0) == 1) != 0;
    return on;
}
// GXV_DIRECT_DISPLAY=1 (direct24 r3): the culled covering rectangle is marked (DrawRec::coverCull) so that the back end can tell a picture starts by overwriting the colour whole.
bool DirectDisplayMark() {
    const bool on = VGX_KNOB(vgx::dd::EffectiveMode(EnvInt("GXV_DIRECT_DISPLAY", 0), EnvInt("GXV_HRW", 0), EnvInt("GXV_DIRECT_DISPLAY_HRW", 0), EnvInt("GXV_DD_DYN", 0)) == 1) != 0; // (sg12: not under GXV_HRW)
    return on;
}
// ---- GXV_CLEARQ (pixcuts25): the game's per-picture EFB clear quad drawn as a constant clear
// MainLoopFramework::clearEfb draws a screen quad (one TEV stage: C0 / A0 straight to the output, GX_ZT_REPLACE with a one-value Z texture, z ALWAYS with
// update, no blend, no alpha test, GX_CULL_BACK) that opens the EFB's first scene of every picture: a full-screen pass of the generated fragment shader
// ("fs 1172", 17 statements, 0.62-0.77 ms in the pixels25 dumps) whose result is one constant colour, one constant alpha and one constant depth. With the
// switch on the front end marks such a draw (DrawRec::clearQ) when ALL of these hold, and the back end (vgx_backend_gl.cpp ClearQTry) then makes the same
// pixels with a glClear of colour, alpha and depth (the cheap clear program, and with GXV_COLOR_LOADSKIP bit 1 + 16 the scene's tiles START with that colour):
//   - a quad (4 vertices, GX_QUADS) whose rectangle covers the whole EFB inside the scissor, wound so that it is drawn (DrawRec::cover / coverCull);
//   - render state: depth test ALWAYS with write, no blend, colour and alpha written, no destination-alpha override, no stencil work (GXV_SHADOW_STENCIL
//     annotates its own window-opening quad: ClearQTry refuses a record with stOp);
//   - key: one TEV stage whose colour is exactly C0 (a = b = c = ZERO, d = C0, or a = C0 with b = c = d = ZERO), bias 0, scale 1, clamp, into PREV, and
//     the alpha exactly A0 likewise; no indirect stage, fog, logic op, alpha test that can fail; GX_ZT_REPLACE with a one-value Z texture (BpZTexConstant) so that the
//     depth is the state's constant (zNear == zFar), and that constant is exactly 0.0 or 1.0;
//   - every byte of the colour, and the alpha (the destination-alpha constant when the key writes it, else A0), is 0 or 255 and the registers are in 0..255:
//     a float that the quad's (half precision) arithmetic and the clear's float agree on in any conversion to 8 bits.
// Then the quad writes colour C, alpha A and depth Z to every pixel of the EFB, which is what glClear(colour | depth) with those values writes (the clear
// ignores the viewport and the scissor is off for it: ClearEfb). Exact for the pictures; what differs is the program that makes them.
//   GXV_CLEARQ=0 (default) off; 1 on; 2 count only ("clearq:" line: recognised, refused by reason; nothing is changed).
// Stage-settable (stagepreset25: "clearq"). No shader text changes with it: the shipped pack is untouched.
int ClearQMode() {
    const int m = static_cast<int>(PORT_STAGE_KNOB(int32_t, std::clamp<long>(port::stageknob::Int("GXV_CLEARQ", 0), 0, 3)));
    return m;
}
// GXV_SILH_GATE (shadow25), read once, stage-settable: 0 off, 1 on, 2 census only.
int SilhGateMode() {
    const int m = static_cast<int>(PORT_STAGE_KNOB(int32_t, std::clamp<long>(port::stageknob::Int("GXV_SILH_GATE", 0), 0, 2)));
    return m;
}
// GXV_SILH_OCC (across26), read once, stage-settable: see vgx_silhocc.h. 0 off, 1 / 2 the gate also needs the occlusion query's all-clear, 3 census.
int SilhOccMode() {
    const int m = static_cast<int>(PORT_STAGE_KNOB(int32_t, std::clamp<long>(port::stageknob::Int("GXV_SILH_OCC", 0), 0, 3)));
    return m;
}
silhocc::History& SilhOccHistory() {
    static silhocc::History h;
    return h;
}
silhocc::Params SilhOccParams() {
    static const long minPx = std::clamp<long>(EnvInt("GXV_SILH_OCC_MINPX", 1), 1, 100000); // samples that count as "he shows" (a pixel is 1)
    static const long hold = std::clamp<long>(EnvInt("GXV_SILH_OCC_HOLD", 0), 0, 600);      // 0: the level's own (12 / 5)
    silhocc::Params p = silhocc::ParamsFor(SilhOccMode(), static_cast<uint32_t>(minPx));
    if (hold > 0) p.hold = static_cast<int>(hold);
    return p;
}
namespace {
uint8_t g_silhHintNext = 0; // (PORT_GX_MARK_SILH_HINT seen; the next EFB copy carries it)
} // namespace
void SilhHintMarker(uint32_t v) { g_silhHintNext = static_cast<uint8_t>(v); }

// ---- GXV_BLOOM_BRACKET (bloom37, vgx_bloombracket.h): the normal bloom's scene bracket when no glow source drew. The game's markers (PORT_GX_MARK_BLOOM_BRACKET, sub-kind in
// bits 8-15) tag the bracket's capture (CopyTexCmd::bracket = the bracket's id of this picture, 1..63), its fill's draws and its put-back's draws (DrawRec::owner = bb::OwnerTag, no
// merge with another owner) and say "dead"; BloomBracketPass (FrontPresent, first of the passes) checks every dead bracket and, with GXV_BLOOM_BRACKET=1, leaves the three commands out.
// =2 counts only. Not under GXV_HRW (the pairs use the owner field; the markers are ignored and counted).
int BloomBracketMode() { return static_cast<int>(VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_BLOOM_BRACKET", 0), 0, 2)))); }
// GXV_TAILFOLD (tail37 / tailfold39; vgx_tailfold.h, vgx_backend_gl.cpp "namespace tailfoldbe"): 0 off (default), 1 / 2 count, 3 apply. Read once. With 3 the game's bracket markers are written
// too (they tag the bloom's put-back draw, the one draw the fold may leave out), without GXV_BLOOM_BRACKET: the markers change nothing but the draws' owner (no merge with another owner).
int TailFoldMode() {
    static const int m = static_cast<int>(std::clamp<long>(EnvInt("GXV_TAILFOLD", 0), 0, 3));
    return m;
}
namespace {
struct BbFront { bool capNext = false; unsigned seq = 0, cur = 0; uint64_t deadMask = 0; } g_bb;
bb::Census g_bbCensus;
}
// (tests) pictures with a bracket, brackets, dead, dropped (=2: would be), then the refusals by bb::Reason (index 4 + reason, kCount entries)
void BloomBracketCounters(uint64_t out[4 + bb::kCount]) {
    out[0] = g_bbCensus.pictures; out[1] = g_bbCensus.brackets; out[2] = g_bbCensus.dead; out[3] = g_bbCensus.dropped;
    for (int r = 0; r < bb::kCount; ++r) out[4 + r] = g_bbCensus.reason[r];
}
void BloomBracketMarker(uint32_t sub) {
    if (BloomBracketMode() == 0 && TailFoldMode() != 3) return;
    J3n2Flush(); // (GXV_J3DNATIVE=4 / 5: a chain still active ends here, so that its draws carry the owner they were sent under)
    if (HrwMode() != 0) { ++g_bbCensus.markerRefusedHrw; return; }
    switch (sub) {
    case bb::kSubCapture:
        g_bb.cur = (g_bb.seq % 63u) + 1u;
        ++g_bb.seq;
        g_bb.deadMask &= ~(1ull << g_bb.cur);
        g_bb.capNext = true;
        break;
    case bb::kSubFillBegin: if (g_bb.cur) g_ownerKey = bb::OwnerTag(g_bb.cur, 1); break;
    case bb::kSubPutBegin: if (g_bb.cur) g_ownerKey = bb::OwnerTag(g_bb.cur, 2); break;
    case bb::kSubFillEnd: case bb::kSubPutEnd: g_ownerKey = 0; break;
    case bb::kSubDead: if (g_bb.cur) g_bb.deadMask |= 1ull << g_bb.cur; break;
    default: break;
    }
}
// hrw41: PORT_GX_MARK_SCENE_DEPTH seen (the game: the 3D scene's depth is complete; its image effects and its own depth clear follow): every draw and EFB copy
// of this picture made from here on carries depthMark, and the back end makes the pair's depth mask just before the first of them that it finds outside a pass
// (vgx_backend_gl.cpp PlanFrame / MaskMid). Every one, not only the next: a pass of the front end may replace the first records after the marker (the folded
// bloom chain makes its own copies), and the mask's place must survive that.
bool g_depthMarkNext = false;
void SceneDepthMarker() {
    J3n2Flush(); // (as OwnerMarker: a chain still active ends here, so that the mark lands on the first draw made after it)
    g_depthMarkNext = true;
}
namespace {
struct ClearQFront {
    uint64_t seen = 0, marked = 0, why[8] = {};
    uint64_t colourMarked = 0, depthOnlyMarked = 0, colourOnlyMarked = 0, colourDepthMarked = 0;
    uint64_t stateWhy[8] = {}; // which render-state rule refused: 0 depth test on but writes nothing needed (func), 1 alpha write differs from colour write, 2 dst-alpha-zero, 3 scissor, 4 depth write on but test off / z not a constant (see CQS_*), 5 colour+alpha off without a depth write
    struct Col { uint32_t v = 0; uint64_t n = 0; } seenCol[4], refusedCol[4]; // the first 4 distinct (r | g << 8 | b << 16 | a << 24) of covering quads that reached the colour rule: all, and those refused
    uint64_t otherCol = 0, otherRefused = 0;
} g_cqf;
enum { CQW_KEY = 1, CQW_STATE, CQW_COLOR, CQW_DEPTH, CQW_ALPHA, CQW_EXTENT };
enum { CQS_DEPTHFUNC = 0, CQS_ALPHAPLANE, CQS_DSTALPHAZERO, CQS_SCISSOR, CQS_ZWRITE, CQS_NOPLANE };
void CqNote(ClearQFront::Col* t, uint64_t& other, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        if (t[i].n && t[i].v == v) { ++t[i].n; return; }
    for (int i = 0; i < 4; ++i)
        if (!t[i].n) { t[i].v = v; t[i].n = 1; return; }
    ++other;
}
uint32_t g_cqRgba = 0; // the colour of the quad ClearQMark last marked (the job carries it: DrawJob::clearRgba)
} // namespace
void ClearQFrontCounters(uint64_t out[8]) {
    out[0] = g_cqf.seen; out[1] = g_cqf.marked;
    for (int i = 1; i < 7; ++i) out[1 + i] = g_cqf.why[i];
}
// The census text of the "clearq:" line: what was marked (colour + depth, colour only, depth only), the render-state refusals by rule, and the first
// four distinct colours (red green blue alpha, as bytes) of the covering quads that reached the colour rule, and of those refused.
int ClearQFrontCensus(char* buf, unsigned cap) {
    unsigned n = 0;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    // fmt1131: every use passes a literal (a lambda template cannot carry the format attribute)
    auto add = [&](const char* fmt, auto... a) { if (n < cap) n += static_cast<unsigned>(std::snprintf(buf + n, cap - n, fmt, a...)); };
#pragma GCC diagnostic pop
    add("marked: colour+depth %u, colour only %u, depth only %u; state refusals: depth-func %u, alpha-plane %u, dst-alpha-zero %u, scissor %u, z-write %u, no-plane %u; colours seen:",
        static_cast<unsigned>(g_cqf.colourDepthMarked), static_cast<unsigned>(g_cqf.colourOnlyMarked), static_cast<unsigned>(g_cqf.depthOnlyMarked),
        static_cast<unsigned>(g_cqf.stateWhy[0]), static_cast<unsigned>(g_cqf.stateWhy[1]), static_cast<unsigned>(g_cqf.stateWhy[2]), static_cast<unsigned>(g_cqf.stateWhy[3]),
        static_cast<unsigned>(g_cqf.stateWhy[4]), static_cast<unsigned>(g_cqf.stateWhy[5]));
    for (const auto& c : g_cqf.seenCol)
        if (c.n) add(" (%u,%u,%u,%u)x%u", c.v & 255u, (c.v >> 8) & 255u, (c.v >> 16) & 255u, c.v >> 24, static_cast<unsigned>(c.n));
    add(" +%u other; refused:", static_cast<unsigned>(g_cqf.otherCol));
    for (const auto& c : g_cqf.refusedCol)
        if (c.n) add(" (%u,%u,%u,%u)x%u", c.v & 255u, (c.v >> 8) & 255u, (c.v >> 16) & 255u, c.v >> 24, static_cast<unsigned>(c.n));
    add(" +%u other", static_cast<unsigned>(g_cqf.otherRefused));
    return static_cast<int>(n);
}
// The job's cover bits for a rectangle quad (4 vertices, GX_QUADS) that covers the EFB: 8 (a clear) | 16 (depth only: colour and alpha are not written), or 0;
// the colour and alpha bytes are left in g_cqRgba (r | g << 8 | b << 16 | a << 24; a colour-only or colour+depth clear's).
// The rules (the comment above): the picture the quad writes is one constant colour/alpha (the registers C0 / A0 clamped to a byte, or the destination-alpha
// constant), with or without one constant depth (0 or 1); the clear writes the same bytes. A quad that writes neither colour nor alpha is a depth-only clear:
// then only the depth rules apply. Colour written without alpha, or the reverse, is not taken (no clear has that mask).
uint8_t ClearQMark(const FsKey& k, const RenderState& rs, const EfbRect& box) {
    ++g_cqf.seen;
    static const bool brk2 = diag::Int("GXV_CLEARQ_BREAK", 0) == 2; // (desktop diagnostic: every covering quad taken by its extent alone, colour rounded - wrong on purpose)
    if (brk2) {
        g_cqRgba = 0;
        for (int c = 0; c < 4; ++c) g_cqRgba |= (g.tevReg[1][c] >= 128 ? 255u : 0u) << (8 * c);
        return 8u;
    }
    auto no = [](int why) -> uint8_t { ++g_cqf.why[why]; return 0; };
    auto noState = [&](int sub) -> uint8_t { ++g_cqf.stateWhy[sub]; return no(CQW_STATE); };
    if (box.x > 0 || box.y > 0 || box.x + box.w < g.efbW || box.y + box.h < g.efbH) return no(CQW_EXTENT);
    if (rs.blend || rs.dstAlphaZero) return noState(CQS_DSTALPHAZERO);
    if (rs.scX > 0 || rs.scY > 0 || rs.scX + rs.scW < g.efbW || rs.scY + rs.scH < g.efbH) return noState(CQS_SCISSOR);
    if (rs.colorWrite != rs.alphaWrite) return noState(CQS_ALPHAPLANE);
    const bool plane = rs.colorWrite && rs.alphaWrite;
    const bool zWrite = rs.depthTest && (rs.depthFunc & 7) == CMP_ALWAYS && rs.depthWrite;
    if (rs.depthTest && (rs.depthFunc & 7) != CMP_ALWAYS) return noState(CQS_DEPTHFUNC); // (a test that can fail)
    if (!plane && !zWrite) return noState(CQS_NOPLANE); // (writes nothing)
    // (the generated text must not be able to discard or reject)
    if (k.numIndStages != 0 || k.rejectZero() || k.atPass()) return no(CQW_KEY);
    if (k.alphaTestOn && !AlphaTestAlwaysPasses(k.alphaTest & 7, -1, (k.alphaTest >> 6) & 3, (k.alphaTest >> 3) & 7, -1)) return no(CQW_KEY);
    if (zWrite) {
        // (GX_ZT_REPLACE with a one-value texture: BuildRenderState collapses the depth range to that value, which is all the generated text relies on: FsKey::ztex is never set)
        if (bits(g.bp[0xF5], 2, 2) != 2 || !(rs.zNear == rs.zFar && (rs.zNear == 0.0f || rs.zNear == 1.0f))) return no(CQW_DEPTH);
    }
    uint32_t rgba = 0;
    if (plane) {
        if (k.numStages != 1 || k.fog || k.logicSrc || k.ztex || k.alphaDead()) return no(CQW_KEY);
        const FsKey::Stage& st = k.stage[0];
        const uint32_t cc = st.color & 0xFFFFFFu, ca = st.alpha & 0xFFFFF0u;
        const uint32_t colD = (15u << 12) | (15u << 8) | (15u << 4) | 2u | (1u << 19), colA = (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19);
        const uint32_t alD = (7u << 13) | (7u << 10) | (7u << 7) | (1u << 4) | (1u << 19), alA = (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19);
        if ((cc != colD && cc != colA) || (ca != alD && ca != alA)) return no(CQW_KEY);
        // The registers are signed 11-bit values: input D (the game's) is read whole and the stage clamps it to 0..255 (the 19th bit of the word: clamp);
        // input A would read the low 8 bits, which differ from the value outside 0..255, so that is refused there.
        const bool aIn = cc == colA, alA2 = ca == alA;
        int v[4] = {g.tevReg[1][0], g.tevReg[1][1], g.tevReg[1][2], g.tevReg[1][3]};
        for (int c = 0; c < 3; ++c) {
            if (aIn && (v[c] < 0 || v[c] > 255)) { CqNote(g_cqf.seenCol, g_cqf.otherCol, 0); return no(CQW_COLOR); }
            v[c] = std::clamp(v[c], 0, 255);
        }
        // the alpha the picture gets: the key's destination-alpha constant (8 bits), else A0 (clamped; input A would wrap it)
        const bool dstOn = bits(g.bp[0x42], 8, 1) != 0;
        if (dstOn && !k.dstAlpha) return no(CQW_ALPHA); // (a destination alpha the key does not write is not understood)
        int a = k.dstAlpha ? static_cast<int>(bits(g.bp[0x42], 0, 8)) : v[3];
        if (!k.dstAlpha && alA2 && (a < 0 || a > 255)) return no(CQW_ALPHA);
        a = std::clamp(a, 0, 255);
        rgba = static_cast<uint32_t>(v[0]) | (static_cast<uint32_t>(v[1]) << 8) | (static_cast<uint32_t>(v[2]) << 16) | (static_cast<uint32_t>(a) << 24);
        CqNote(g_cqf.seenCol, g_cqf.otherCol, rgba);
        // (GXV_CLEARQ=1 takes any constant colour; GXV_CLEARQ=3 keeps the old rule, every byte 0 or 255, for a console A/B of the rounding)
        if (ClearQMode() == 3) {
            for (int c = 0; c < 4; ++c) {
                const uint32_t b8 = (rgba >> (8 * c)) & 255u;
                if (b8 != 0 && b8 != 255) { CqNote(g_cqf.refusedCol, g_cqf.otherRefused, rgba); return no(CQW_COLOR); }
            }
        }
        ++g_cqf.colourMarked;
    }
    g_cqRgba = rgba;
    ++g_cqf.marked;
    if (plane && zWrite) ++g_cqf.colourDepthMarked;
    else if (plane) ++g_cqf.colourOnlyMarked;
    else ++g_cqf.depthOnlyMarked;
    // bit 3: a clear; bit 4: depth only (no colour, no alpha); bit 5: no depth (colour and alpha only)
    return static_cast<uint8_t>(8u | (plane ? 0u : 16u) | (zWrite ? 0u : 32u));
}

namespace {
// The cuts of GXV_SHADOWFILL_BOUNDS (the fill scissored / left out, a capture of nothing made a fill of 0): that switch, or GXV_SHADOW_RECT=1.
bool ShadowFillCuts() {
    const bool on = PORT_STAGE_KNOB(int32_t, port::stageknob::Int("GXV_SHADOWFILL_BOUNDS", 1) != 0 || ShadowRectMode() == 1) != 0;
    return on;
}
} // namespace
namespace {
// The draw may write an alpha other than the constant 0 (the post pass classifies again from the record, ShadowFillAlphaKind;
// this test only decides whether bounds are worked out, so it may say yes more often).
inline bool SfAlphaOther() {
    const RenderState& rs = cur.rs;
    if (!rs.alphaWrite) return false;
    if (rs.blend && rs.dstAlphaZero == 1) return false;
    if (!rs.blend && rs.dstAlphaZero == 0 && cur.fs.dstAlpha && bits(g.bp[0x42], 0, 8) == 0) return false;
    return true;
}
// Points (model space) through 3 x 4 rows, the projection registers and the current viewport, into an EFB rectangle; each point
// widened by a bound on the single-precision rounding of the GPU's own sums (1e-5 of the magnitudes summed, about 170 units in
// the last place) and the whole rectangle by kSfPad pixels (pixel centres, sub-pixel snapping, the viewport's rounding to render
// pixels and vitaGL's odd-size viewport). In doubles.
constexpr int kSfPad = 3;
struct SfProj {
    double p[4];
    bool ortho;
    double vx, vy, vw, vh;
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    bool bad = false;
    SfProj() {
        for (int i = 0; i < 4; ++i) p[i] = u2f(g.xfReg[0x20 + i]);
        ortho = (g.xfReg[0x26] & 1) != 0;
        vx = cur.rs.vpX; vy = cur.rs.vpY; vw = cur.rs.vpW; vh = cur.rs.vpH;
    }
    void Point(const float* m, double x, double y, double z) {
        if (bad) return;
        double e[3], a[3];
        for (int r = 0; r < 3; ++r) {
            const double m0 = m[r * 4], m1 = m[r * 4 + 1], m2 = m[r * 4 + 2], m3 = m[r * 4 + 3];
            e[r] = m0 * x + m1 * y + m2 * z + m3;
            a[r] = std::fabs(m0 * x) + std::fabs(m1 * y) + std::fabs(m2 * z) + std::fabs(m3);
        }
        Tail(e, a);
    }
    // (GXV_SHADOW_RECT_FE) Point with the matrix already in doubles: the same products and sums, in the same order, so the same numbers.
    void PointD(const double* m, double x, double y, double z) {
        if (bad) return;
        double e[3], a[3];
        for (int r = 0; r < 3; ++r) {
            const double m0 = m[r * 4], m1 = m[r * 4 + 1], m2 = m[r * 4 + 2], m3 = m[r * 4 + 3];
            e[r] = m0 * x + m1 * y + m2 * z + m3;
            a[r] = std::fabs(m0 * x) + std::fabs(m1 * y) + std::fabs(m2 * z) + std::fabs(m3);
        }
        Tail(e, a);
    }
    // e = the eye-space position, a = the sums of the magnitudes that made it (the rounding bound's scale): the projection, the screen
    // rectangle widened by the rounding bound, joined to this object's rectangle.
    void Tail(const double* e, const double* a) {
        // (BuildProjC: x = p0 ex + p1 ez, y = -(p2 ey + p3 ez), w = -ez; orthographic: x = p0 ex + p1, y = -(p2 ey + p3), w = 1;
        // the EFB row is vy + (1 - y_gx / w) vh / 2 with y_gx = p2 ey + p3 (ez), as SmallDrawBounds maps it)
        double cx, cy, cw, ax, ay, aw;
        if (ortho) {
            cx = p[0] * e[0] + p[1]; ax = std::fabs(p[0]) * a[0] + std::fabs(p[1]);
            cy = p[2] * e[1] + p[3]; ay = std::fabs(p[2]) * a[1] + std::fabs(p[3]);
            cw = 1.0; aw = 0.0;
        } else {
            cx = p[0] * e[0] + p[1] * e[2]; ax = std::fabs(p[0]) * a[0] + std::fabs(p[1]) * a[2];
            cy = p[2] * e[1] + p[3] * e[2]; ay = std::fabs(p[2]) * a[1] + std::fabs(p[3]) * a[2];
            cw = -e[2]; aw = a[2];
        }
        constexpr double kG = 1e-5;
        const double dx = kG * ax + 1e-7, dy = kG * ay + 1e-7, dw = kG * aw + 1e-7;
        if (!(cw > 8.0 * dw) || !std::isfinite(cx) || !std::isfinite(cy)) { bad = true; return; } // (behind the eye, or too near it)
        const double nx = cx / cw, ny = cy / cw;
        const double ex = (dx + std::fabs(nx) * dw) / (cw - dw), ey = (dy + std::fabs(ny) * dw) / (cw - dw);
        const double sx = vx + (nx + 1.0) * 0.5 * vw, sy = vy + (1.0 - ny) * 0.5 * vh;
        const double rx = ex * 0.5 * std::fabs(vw), ry = ey * 0.5 * std::fabs(vh);
        if (!(std::fabs(sx) + rx < 16384.0 && std::fabs(sy) + ry < 16384.0)) { bad = true; return; }
        x0 = std::min(x0, sx - rx); x1 = std::max(x1, sx + rx);
        y0 = std::min(y0, sy - ry); y1 = std::max(y1, sy + ry);
    }
    // (GXV_SHADOW_RECT_FE) Box with each product of the matrix and one coordinate made once (6 a row, not 8 x 3): the corner's sums are
    // formed in the order Point forms them (((m0 x + m1 y) + m2 z) + m3), from the same products, so every corner gives the same numbers.
    void BoxFast(const double* m, const RowBox& b) {
        if (bad) return;
        double X[3][2], Y[3][2], Z[3][2], AX[3][2], AY[3][2], AZ[3][2], M3[3];
        for (int r = 0; r < 3; ++r) {
            M3[r] = m[r * 4 + 3];
            for (int s = 0; s < 2; ++s) {
                X[r][s] = m[r * 4] * static_cast<double>(s ? b.mx[0] : b.mn[0]);
                Y[r][s] = m[r * 4 + 1] * static_cast<double>(s ? b.mx[1] : b.mn[1]);
                Z[r][s] = m[r * 4 + 2] * static_cast<double>(s ? b.mx[2] : b.mn[2]);
                AX[r][s] = std::fabs(X[r][s]); AY[r][s] = std::fabs(Y[r][s]); AZ[r][s] = std::fabs(Z[r][s]);
            }
        }
        const double a3[3] = {std::fabs(M3[0]), std::fabs(M3[1]), std::fabs(M3[2])};
        for (int k = 0; k < 8 && !bad; ++k) {
            const int sx = k & 1, sy = (k >> 1) & 1, sz = (k >> 2) & 1;
            double e[3], a[3];
            for (int r = 0; r < 3; ++r) {
                e[r] = X[r][sx] + Y[r][sy] + Z[r][sz] + M3[r];
                a[r] = AX[r][sx] + AY[r][sy] + AZ[r][sz] + a3[r];
            }
            Tail(e, a);
        }
    }
    void Box(const float* m, const RowBox& b) {
        for (int k = 0; k < 8 && !bad; ++k) Point(m, (k & 1) ? b.mx[0] : b.mn[0], (k & 2) ? b.mx[1] : b.mn[1], (k & 4) ? b.mx[2] : b.mn[2]);
    }
    // The rectangle, cut to the draw's scissor (an empty one has w or h 0).
    bool Rect(EfbRect& r) const {
        if (bad || !(x0 <= x1 && y0 <= y1)) return false;
        const RenderState& rs = cur.rs;
        const int bx0 = static_cast<int>(std::floor(x0)) - kSfPad, by0 = static_cast<int>(std::floor(y0)) - kSfPad;
        const int bx1 = static_cast<int>(std::ceil(x1)) + kSfPad, by1 = static_cast<int>(std::ceil(y1)) + kSfPad;
        const int ax = std::max<int>(bx0, rs.scX), ay = std::max<int>(by0, rs.scY);
        const int cx1 = std::min<int>(bx1, rs.scX + rs.scW), cy1 = std::min<int>(by1, rs.scY + rs.scH);
        r = EfbRect{static_cast<int16_t>(ax), static_cast<int16_t>(ay), static_cast<int16_t>(cx1 > ax ? cx1 - ax : 0), static_cast<int16_t>(cy1 > ay ? cy1 - ay : 0)};
        return true;
    }
};
// The 3 x 4 position rows the vertex shader uses for matrix row `row` of a per-vertex palette, or null (the shader would clamp it).
inline const float* SfPalRows(uint32_t row) {
    const uint8_t pv = cur.vs.perVtxPos;
    const uint32_t maxRow = pv == 3 ? static_cast<uint32_t>(kPalSmallMaxRow) : (pv == 2 ? static_cast<uint32_t>(kPalShortMaxRow) : 61u);
    return row <= maxRow ? g.xfPos[row] : nullptr;
}
// The current matrix's rows, as BuildMtxC (rows pi..pi+2) or SoftTransform (SoftPosRows) read them, or null where the two differ.
inline const float* SfMtxRows() {
    const uint32_t pi = g.matIdxA & 0x3F;
    return pi <= 61 ? g.xfPos[pi] : nullptr;
}
// ---- GXV_SHADOW_RECT_FE=1 (shadowfe24): the same bounds for less work. What each bound costs (the host fixture's callgrind, Honeyhive-like
// scene with 31 shadow volumes: tests/vgx_fe_work.cpp FEW_SHADOW / FEW_SHADOWDYN): every alpha-writing draw runs SfProj::Point for the 8
// corners of its list's box or for every vertex of its immediate geometry - ~230 instructions a point, ~130 of them double-precision VFP
// operations with four divisions: 600+ cycles on the console - and ShadowVolumeDrawer::draw makes every volume TWICE (ONE / ONE under cull
// front, SUBTRACT under cull back: the same geometry, the same matrix, the same viewport and scissor; only the blend differs). A box volume
// (ShadowVolumeBox::drawShape) sends 3 primitives of 14, 8 and 8 vertices that name 14 different points, 30 vertex sends a pass, 60 a volume.
// Three exact savings, all of which give the numbers the plain code gives (GXV_SHADOW_RECT_FE=2 compares them draw by draw):
//  1. a static run's bound is looked up in a four-entry memo keyed by everything it is a function of (the projection registers, viewport,
//     scissor, the matrix rows, the list's box): the second pass of every volume, and the repeat of a model, is a compare and a copy;
//  2. a dynamic draw's vertices are looked up by their bits in a table of what each position did to the rectangle (its screen extent, widened by
//     the rounding bound), valid for as long as the matrix, projection, viewport and scissor stay the same: a point repeated within a draw, by the
//     draw after it (a box's strip and fans share their points) or by the volume's second pass is not worked out again. The rectangle is a min /
//     max over the points, so joining the stored extents gives the same rectangle as working every point out;
//  3. the matrix is converted to doubles once and a box's products are formed once (SfProj::BoxFast, PointD).
// Not changed: draws with per-vertex matrices (skinned: Mario's silhouette) work out every row's box as before (with 3).
struct SfFeCount { uint64_t hits = 0, misses = 0, dupSkipped = 0, points = 0, keysSkipped = 0, verified = 0, differing = 0; } g_sfFe;
struct SfMemoKey {
    uint32_t xf[7];
    float vp[4];
    int16_t sc[4];
    float m[12];
    float box[6]; // (a static run's box; 0 for immediate geometry)
    uint32_t kind; // 0xFFFFFFFF a static run, 0 immediate geometry
};
struct SfMemo {
    bool valid = false;
    bool known = false; // the result: a rectangle (else: not known)
    SfMemoKey key;
    EfbRect r;
};
// (four entries, the oldest replaced: a model with several shapes is drawn twice shape by shape, so the second pass's first run is not the last
// result but the one before the last few)
constexpr int kSfMemoWays = 4;
SfMemo g_sfMemo[kSfMemoWays];
int g_sfMemoNext = 0;
const SfMemo* SfMemoFind(const SfMemoKey& key) {
    for (int i = 0; i < kSfMemoWays; ++i) {
        const SfMemo& e = g_sfMemo[i];
        if (e.valid && std::memcmp(&e.key, &key, sizeof key) == 0) return &e;
    }
    return nullptr;
}
SfMemo& SfMemoSlot() {
    SfMemo& e = g_sfMemo[g_sfMemoNext];
    g_sfMemoNext = (g_sfMemoNext + 1) & (kSfMemoWays - 1);
    return e;
}
// What one position did to the rectangle (SfProj::Tail's four updates), for the dynamic draws' vertices: 64 slots, two a position, tagged with
// the generation of the context (matrix, projection, viewport, scissor) they were made under; a lost slot costs a point, never a wrong value.
struct SfPt {
    uint32_t w[3];
    uint32_t stamp;
    double x0, x1, y0, y1;
    bool bad;
};
SfPt g_sfPt[64];
uint32_t g_sfPtStamp = 0;
SfMemoKey g_sfPtCtx;
bool g_sfPtCtxValid = false;
inline void SfKeyInit(SfMemoKey& k, const float* m, uint32_t kind) {
    std::memset(&k, 0, sizeof k);
    for (int i = 0; i < 7; ++i) k.xf[i] = g.xfReg[0x20 + i];
    const RenderState& rs = cur.rs;
    k.vp[0] = rs.vpX; k.vp[1] = rs.vpY; k.vp[2] = rs.vpW; k.vp[3] = rs.vpH;
    k.sc[0] = rs.scX; k.sc[1] = rs.scY; k.sc[2] = rs.scW; k.sc[3] = rs.scH;
    std::memcpy(k.m, m, sizeof k.m);
    k.kind = kind;
}
inline void SfApply(split::DrawJob& j, bool known, const EfbRect& r) {
    if (!known) return;
    j.ab = 1;
    j.abX = r.x; j.abY = r.y; j.abW = r.w; j.abH = r.h;
}
// The plain code's result for a dynamic draw (the vertices as SfDynamicBounds reads them), or `false` for "return without a result".
bool SfDynamicPlain(const uint8_t* v, uint32_t count, uint32_t stride, bool perVtx, const float* m0, bool& known, EfbRect& r) {
    SfProj P;
    const float* m = m0;
    for (uint32_t i = 0; i < count && !P.bad; ++i, v += stride) {
        float q[4];
        std::memcpy(q, v, sizeof q);
        if (perVtx) {
            if (!(q[3] >= 0.0f && q[3] < 64.0f)) return false;
            m = SfPalRows(static_cast<uint32_t>(q[3] + 0.5f));
            if (!m) return false;
        }
        P.Point(m, q[0], q[1], q[2]);
    }
    known = P.Rect(r);
    return true;
}
// Bounds of a dynamic draw from its decoded vertices (model space, positions with the matrix row in w).
void SfDynamicBounds(split::DrawJob& j, const uint8_t* v, uint32_t count, uint32_t stride) {
    if (!SfBoundsWanted() || !SfAlphaOther() || j.prim != PRIM_TRIANGLES) return;
    const bool perVtx = cur.vs.perVtxPos != 0;
    if (perVtx && cur.soft) return;
    const float* m = SfMtxRows();
    if (!perVtx && !m) return;
    const int fe = ShadowRectFeMode();
    if (fe == 0 || perVtx) { // (as it was; with per-vertex matrices every vertex has its own rows)
        bool known = false;
        EfbRect r;
        if (!SfDynamicPlain(v, count, stride, perVtx, m, known, r)) return;
        SfApply(j, known, r);
        return;
    }
    SfMemoKey key;
    SfKeyInit(key, m, 0);
    if (!g_sfPtCtxValid || std::memcmp(&g_sfPtCtx, &key, sizeof key) != 0) { // (another context: nothing made before applies)
        g_sfPtCtx = key;
        g_sfPtCtxValid = true;
        if (++g_sfPtStamp == 0) { std::memset(g_sfPt, 0, sizeof g_sfPt); g_sfPtStamp = 1; }
    }
    SfProj P;
    bool haveMd = false;
    double md[12];
    SfProj Q = P; // (one position at a time: reset before each)
    const uint8_t* q = v;
    for (uint32_t i = 0; i < count && !P.bad; ++i, q += stride) {
        uint32_t w[3];
        std::memcpy(w, q, 12);
        const uint32_t h = ((w[0] * 0x9E3779B1u) ^ (w[1] * 0x85EBCA6Bu) ^ (w[2] * 0xC2B2AE35u)) >> 26;
        SfPt* hit = nullptr;
        SfPt& a = g_sfPt[h];
        SfPt& b = g_sfPt[h ^ 1u];
        if (a.stamp == g_sfPtStamp && a.w[0] == w[0] && a.w[1] == w[1] && a.w[2] == w[2]) hit = &a;
        else if (b.stamp == g_sfPtStamp && b.w[0] == w[0] && b.w[1] == w[1] && b.w[2] == w[2]) hit = &b;
        if (hit) {
            ++g_sfFe.dupSkipped;
            if (hit->bad) P.bad = true;
            else { P.x0 = std::min(P.x0, hit->x0); P.x1 = std::max(P.x1, hit->x1); P.y0 = std::min(P.y0, hit->y0); P.y1 = std::max(P.y1, hit->y1); }
            continue;
        }
        if (!haveMd) { for (int k = 0; k < 12; ++k) md[k] = m[k]; haveMd = true; }
        float f[3];
        std::memcpy(f, w, 12);
        Q.x0 = 1e30; Q.y0 = 1e30; Q.x1 = -1e30; Q.y1 = -1e30; Q.bad = false;
        Q.PointD(md, f[0], f[1], f[2]);
        ++g_sfFe.points;
        SfPt& put = a.stamp == g_sfPtStamp ? b : a;
        put.w[0] = w[0]; put.w[1] = w[1]; put.w[2] = w[2]; put.stamp = g_sfPtStamp;
        put.x0 = Q.x0; put.x1 = Q.x1; put.y0 = Q.y0; put.y1 = Q.y1; put.bad = Q.bad;
        if (Q.bad) P.bad = true;
        else { P.x0 = std::min(P.x0, Q.x0); P.x1 = std::max(P.x1, Q.x1); P.y0 = std::min(P.y0, Q.y0); P.y1 = std::max(P.y1, Q.y1); }
    }
    EfbRect r;
    const bool known = P.Rect(r);
    if (fe == 2) { // (both ways: the plain code's rectangle is the one used)
        bool k2 = false;
        EfbRect r2;
        const bool ret = SfDynamicPlain(v, count, stride, false, m, k2, r2);
        ++g_sfFe.verified;
        if (!ret || k2 != known || (known && (r2.x != r.x || r2.y != r.y || r2.w != r.w || r2.h != r.h))) ++g_sfFe.differing;
        if (ret) SfApply(j, k2, r2);
        return;
    }
    SfApply(j, known, r);
}
// Bounds of a static run from its list's bounds (SetPendingBounds: box 0 holds every vertex, boxes 1.. one matrix row each).
void SfStaticBounds(split::DrawJob& j, const RowBox* pb, uint32_t pn) {
    if (!SfBoundsWanted() || !pb || !pn || !SfAlphaOther() || j.prim != PRIM_TRIANGLES) return;
    const int fe = ShadowRectFeMode();
    if (cur.vs.perVtxPos == 0) {
        const float* m = SfMtxRows();
        if (!m) return;
        if (fe == 0) {
            SfProj P;
            P.Box(m, pb[0]);
            EfbRect r;
            if (!P.Rect(r)) return;
            SfApply(j, true, r);
            return;
        }
        SfMemoKey key;
        SfKeyInit(key, m, 0xFFFFFFFFu); // (kind -1: a static run's key)
        std::memcpy(key.box, pb[0].mn, 12);
        std::memcpy(key.box + 3, pb[0].mx, 12);
        bool known = false;
        EfbRect r;
        if (const SfMemo* hit = SfMemoFind(key)) {
            ++g_sfFe.hits;
            known = hit->known; r = hit->r;
        } else {
            ++g_sfFe.misses;
            double md[12];
            for (int i = 0; i < 12; ++i) md[i] = m[i];
            SfProj P;
            P.BoxFast(md, pb[0]);
            g_sfFe.points += 8;
            known = P.Rect(r);
            SfMemo& e = SfMemoSlot();
            e.valid = true; e.known = known; e.key = key; e.r = r;
        }
        if (fe == 2) {
            SfProj P2;
            P2.Box(m, pb[0]);
            EfbRect r2;
            const bool k2 = P2.Rect(r2);
            ++g_sfFe.verified;
            if (k2 != known || (known && (r2.x != r.x || r2.y != r.y || r2.w != r.w || r2.h != r.h))) ++g_sfFe.differing;
            SfApply(j, k2, r2);
            return;
        }
        SfApply(j, known, r);
        return;
    }
    // (per-vertex matrices: every row's box, as it was; the products shared within a box with GXV_SHADOW_RECT_FE)
    if (pn < 2) return;
    SfProj P;
    for (uint32_t i = 1; i < pn && !P.bad; ++i) {
        const float* m = SfPalRows(pb[i].row);
        if (!m) return;
        if (fe == 0 || fe == 2) P.Box(m, pb[i]);
        else {
            double md[12];
            for (int q = 0; q < 12; ++q) md[q] = m[q];
            P.BoxFast(md, pb[i]);
            g_sfFe.points += 8;
        }
    }
    EfbRect r;
    if (!P.Rect(r)) return;
    if (fe == 2) { // (the same rows through the products-shared box: compared)
        SfProj Q;
        for (uint32_t i = 1; i < pn && !Q.bad; ++i) {
            double md[12];
            const float* m = SfPalRows(pb[i].row);
            for (int q = 0; q < 12; ++q) md[q] = m[q];
            Q.BoxFast(md, pb[i]);
        }
        EfbRect r2;
        const bool k2 = Q.Rect(r2);
        ++g_sfFe.verified;
        if (!k2 || r2.x != r.x || r2.y != r.y || r2.w != r.w || r2.h != r.h) ++g_sfFe.differing;
    }
    SfApply(j, true, r);
}
} // namespace

// (GXV_STATICCULL / GXV_STATICCULL_PAL / GXV_DYNCULL: the tests; the description is at "static runs outside the view" below)
bool StaticCullEnabled() {
    const bool on = VGX_KNOB(EnvInt("GXV_STATICCULL", 1) != 0) != 0;
    return on;
}
// GXV_STATICCULL_PAL (ta23; read once, default 0): the same test for static runs WITH per-vertex matrices (PNMTXIDX: skinned and
// multi-joint shapes: Mario, the bees, enemies, multi-joint map parts). Each vertex is transformed by the rows its own index names
// (u_pal[row..row+2], the shader's `pi`, which are g.xfPos[row..row+2] for every palette class: the short palettes copy rows 0..K-1 and
// are used only when the run's highest row fits them). The list's bounds hold one box per row (RowBoxesOf: boxes 1..n), so every vertex
// is inside the hull of its row's box through its row's matrix: a run is left out when the boxes of ALL its rows are outside the SAME
// plane (by the margin of each). A row above 61 (where the shader clamps), a missing per-row box or a non-finite number: drawn.
// Needs GXV_STATICCULL (the bounds). Exact for the same reasons as the rigid case.
bool StaticCullPalEnabled() {
    const bool on = VGX_KNOB(EnvInt("GXV_STATICCULL_PAL", 0) != 0) != 0;
    return on;
}
// GXV_DYNCULL (ta23; read once, default 0): dynamic draws (vertices decoded on the CPU into the picture's arena: particles, small shapes,
// lists under the static-buffer limits, immediate geometry) whose vertices ALL lie outside one plane of the clip volume are not drawn,
// the same planes, margins and rules as GXV_STATICCULL, tested on the decoded model-space vertices themselves (tighter than a box) with
// the matrix rows the draw is transformed by (the CPU transform's, BuildMtxC's or the palette's). Only triangles (not lines / points,
// which the vertex shader widens), only when the vertices are already decoded on this thread (GXV_SPLIT_COPY / GXV_FE_PARALLEL decodes
// that are still pending are not forced: such a draw is drawn), only after ResolveState (left out the way `cullAll` leaves a draw out).
//   0 off (as before); N >= 1: on for draws of at least N vertices (1: all; 4 skips nothing of a particle quad). The test costs about
//   20 ns a vertex on the front end and stops at the first vertex inside the volume.
uint32_t DynCullMin() {
    const uint32_t n = static_cast<uint32_t>(VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_DYNCULL", 0), 0, 1 << 20))));
    return n;
}
namespace {
uint64_t g_scRuns = 0, g_scCulled = 0, g_scCulledVerts = 0; // static runs tested, culled, and their vertices (the player's thread)
// (ta23 census for the "ta23:" basic-log line, per log window: static runs with per-vertex matrices tested / culled, the indices of culled static runs, dynamic draws
// tested / culled / their indices / skipped because the vertices were not decoded yet)
struct CullCtr {
    uint64_t runs = 0, culled = 0, idx = 0, palRuns = 0, palCulled = 0, palIdx = 0, dynTested = 0, dynCulled = 0, dynIdx = 0, dynLater = 0, pictures = 0;
    uint64_t vcHad = 0, vcTook = 0, vcIdxHad = 0, vcIdxTook = 0; // (GXV_DLVCACHE) static runs with a reordered block / that drew it; their indices
};
CullCtr g_cc, g_ccTotal;
// The clip volume of the current state, as StaticBoxOutside tests it (see above): the projection, and which side planes may be used.
struct ClipNow {
    float p[6];
    bool ortho, sides;
    float kx, ky, pSum;
};
void ClipNowOfCalc(ClipNow& c);
// GXV_FE24_CLIP: 1 = ClipNowOf is memoised on the 16 words it reads (the result is a pure function of them) and PointOutMask leaves early
// for a point inside the volume; 2 = the same, and every memo hit is checked against a fresh computation (counted, FE24 log).
struct ClipMemo { uint32_t key[16]; ClipNow c; bool ok; };
ClipMemo g_clipMemo;
uint64_t g_fe24ClipHit, g_fe24ClipMiss, g_fe24ClipBad, g_fe24PtEarly, g_fe24PtFull;
int HotprofDynCull(); // (GXV_HOTPROF_DYNCULL, below)
void ClipNowOf(ClipNow& c) {
    int mode = VGX_KNOB(EnvInt("GXV_FE24_CLIP", 0));
    if (mode == 0 && HotprofDynCull() == 1) mode = 1; // (GXV_HOTPROF_DYNCULL=1 takes fe24's memo of the clip volume: a pure function of the 16 words)
    if (mode == 0) { ClipNowOfCalc(c); return; }
    uint32_t k[16];
    k[0] = g.xfReg[0x1A]; k[1] = g.xfReg[0x1B]; k[2] = g.xfReg[0x1D]; k[3] = g.xfReg[0x1E];
    for (int i = 0; i < 7; ++i) k[4 + i] = g.xfReg[0x20 + i];
    k[11] = g.bp[0x59]; k[12] = g.bp[0x20]; k[13] = g.bp[0x21];
    k[14] = static_cast<uint32_t>(g.efbW); k[15] = static_cast<uint32_t>(g.efbH);
    ClipMemo& mm = g_clipMemo;
    if (mm.ok && std::memcmp(mm.key, k, sizeof k) == 0) {
        ++g_fe24ClipHit;
        c = mm.c;
        if (mode == 2) {
            ClipNow f;
            ClipNowOfCalc(f);
            if (std::memcmp(f.p, c.p, sizeof f.p) != 0 || f.ortho != c.ortho || f.sides != c.sides || std::memcmp(&f.kx, &c.kx, 12) != 0) ++g_fe24ClipBad;
        }
        return;
    }
    ++g_fe24ClipMiss;
    ClipNowOfCalc(c);
    std::memcpy(mm.key, k, sizeof k);
    mm.c = c;
    mm.ok = true;
}
void ClipNowOfCalc(ClipNow& c) {
    for (int i = 0; i < 6; ++i) c.p[i] = u2f(g.xfReg[0x20 + i]);
    c.ortho = (g.xfReg[0x26] & 1) != 0;
    // The side planes, only where the scissor (within the EFB) lies inside the viewport, give or take kGuard EFB pixels, and then
    // with the planes moved out by twice that: what is culled is more than kGuard pixels beyond the scissor. A GPU that clipped
    // only against the near plane and the scissor (the GXM's region clip; vitaGL does not clip to the viewport) would draw none
    // of it either, nor would the 1-pixel narrower viewport vitaGL makes of an odd width. (The game's 3D passes: the viewport is
    // the scissor.) Behind the eye is behind the near plane, which every GPU clips.
    constexpr float kGuard = 4.0f;
    const int xoff = static_cast<int>(bits(g.bp[0x59], 0, 10)) * 2, yoff = static_cast<int>(bits(g.bp[0x59], 10, 10)) * 2;
    const float sx0 = std::max(0.0f, static_cast<float>(static_cast<int>(bits(g.bp[0x20], 12, 11)) - xoff));
    const float sy0 = std::max(0.0f, static_cast<float>(static_cast<int>(bits(g.bp[0x20], 0, 11)) - yoff));
    const float sx1 = std::min(static_cast<float>(g.efbW), static_cast<float>(static_cast<int>(bits(g.bp[0x21], 12, 11)) - xoff + 1));
    const float sy1 = std::min(static_cast<float>(g.efbH), static_cast<float>(static_cast<int>(bits(g.bp[0x21], 0, 11)) - yoff + 1));
    const float vsx = u2f(g.xfReg[0x1A]), vsy = u2f(g.xfReg[0x1B]), vox = u2f(g.xfReg[0x1D]), voy = u2f(g.xfReg[0x1E]);
    const float vpW = 2.0f * vsx, vpH = -2.0f * vsy, vpX = vox - vsx - static_cast<float>(xoff), vpY = voy + vsy - static_cast<float>(yoff);
    c.sides = vpW >= 16.0f && vpH >= 16.0f && vpW < 1e5f && vpH < 1e5f && sx0 >= vpX - kGuard && sx1 <= vpX + vpW + kGuard &&
              sy0 >= vpY - kGuard && sy1 <= vpY + vpH + kGuard;
    c.kx = 1.0f + 4.0f * kGuard / (c.sides ? vpW : 1.0f);
    c.ky = 1.0f + 4.0f * kGuard / (c.sides ? vpH : 1.0f); // (2 kGuard pixels in NDC)
    c.pSum = std::fabs(c.p[0]) + std::fabs(c.p[1]) + std::fabs(c.p[2]) + std::fabs(c.p[3]) + 1.0f;
}
// The planes (bit 0 right, 1 left, 2 top, 3 bottom, 4 behind the eye) that every point of the model-space box b, through the position
// rows m (3 rows of 4), is outside of by more than the margin; 0 when the box's centre is inside the volume or a number is not finite.
#include "vgx_hotprof_boxcull.inc" // (BoxOutMaskOld: the test as it was; HpBoxOutMask: GXV_HOTPROF_BOXCULL's; a file of its own for tests/vgx_hotprof_boxcull.cpp)
int HotprofBoxCull(); // (GXV_HOTPROF_BOXCULL, below)
uint64_t g_hpBoxCalls, g_hpBoxBad, g_hpBoxOut;
uint32_t BoxOutMask(const ClipNow& c, const float* const m[3], const RowBox& b) {
    const int hp = HotprofBoxCull();
    if (hp == 0) return BoxOutMaskOld(c, m, b);
    if (hp == 1) return HpBoxOutMask(c, m, b);
    const uint32_t o = BoxOutMaskOld(c, m, b), f = HpBoxOutMask(c, m, b); // (2: verify, the old mask used)
    ++g_hpBoxCalls;
    if (o) ++g_hpBoxOut;
    if (o != f) ++g_hpBoxBad;
    return o;
}
// The same for one model-space point (GXV_DYNCULL): its own margin, from its own magnitudes.
inline uint32_t PointOutMask(const ClipNow& c, const float* const m[3], float x, float y, float z) {
    const float* const p = c.p;
    const float ex = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
    const float ey = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
    const float ez = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
    float cx, cy, cw, a;
    if (c.ortho) {
        cx = p[0] * ex + p[1]; cy = p[2] * ey + p[3]; cw = 1.0f;
        a = std::fabs(p[0] * ex) + std::fabs(p[1]) + std::fabs(p[2] * ey) + std::fabs(p[3]) + 1.0f;
    } else {
        cx = p[0] * ex + p[1] * ez; cy = p[2] * ey + p[3] * ez; cw = -ez;
        a = std::fabs(p[0] * ex) + std::fabs(p[1] * ez) + std::fabs(p[2] * ey) + std::fabs(p[3] * ez) + std::fabs(ez);
    }
    const float tx = cx - c.kx * cw, nx = -cx - c.kx * cw, ty = cy - c.ky * cw, ny = -cy - c.ky * cw;
    // GXV_FE24_CLIP: the margin is positive when finite (and the point is then reported as nothing when it is not), so a point that is
    // not beyond any plane even with a margin of 0 (a NaN compares false) has mask 0 either way: the margin need not be computed.
    if (VGX_KNOB(EnvInt("GXV_FE24_CLIP", 0)) != 0) {
        if (!((c.sides && (::fpb::Active::GtZ(tx) || ::fpb::Active::GtZ(nx) || ::fpb::Active::GtZ(ty) || ::fpb::Active::GtZ(ny))) || (!c.ortho && ::fpb::Active::GtZ(-cw)))) { ++g_fe24PtEarly; return 0; }
        ++g_fe24PtFull;
    }
    const float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
    float erow = 0.0f;
    for (int r = 0; r < 3; ++r)
        erow = ::fpb::Active::Max(erow, std::fabs(m[r][0]) * ax + std::fabs(m[r][1]) * ay + std::fabs(m[r][2]) * az + std::fabs(m[r][3]));
    const float margin = 2.0f * ((1e-4f * erow + 1e-6f) * c.pSum + 1e-4f * a) + 1e-6f;
    if (!::fpb::Active::Lt(margin, 1e30f)) return 0;
    uint32_t o = 0;
    if (c.sides) {
        if (::fpb::Active::Gt(tx, margin)) o |= 1u;
        if (::fpb::Active::Gt(nx, margin)) o |= 2u;
        if (::fpb::Active::Gt(ty, margin)) o |= 4u;
        if (::fpb::Active::Gt(ny, margin)) o |= 8u;
    }
    if (!c.ortho && ::fpb::Active::Gt(-cw, margin)) o |= 16u;
    return o;
}
// Whether every point of the model-space box b, through the current position matrix and projection, is outside one plane
// of the clip volume (see above).
bool StaticBoxOutside(const RowBox& b) {
    const uint32_t pi = g.matIdxA & 0x3F;
    const float* m[3];
    for (uint32_t r = 0; r < 3; ++r) m[r] = g.xfPos[pi + r < 64 ? pi + r : 63]; // (BuildMtxC's rows)
    ClipNow c;
    ClipNowOf(c);
    return BoxOutMask(c, m, b) != 0;
}
// gpu960 (GXV_PIX_CENSUS): the share of the screen a model-space box covers at most: its eight corners through the position matrix and the projection,
// the bounding rectangle in NDC cut to the view; 1 when a corner is at or behind the eye (the box may fill the view). An upper bound of what a static run
// draws (the GPU's own clipping and the depth test only take away).
float BoxScreenFrac(const ClipNow& c, const float* const m[3], const RowBox& b) {
    const float* const p = c.p;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int k = 0; k < 8; ++k) {
        const float x = (k & 1) ? b.mx[0] : b.mn[0], y = (k & 2) ? b.mx[1] : b.mn[1], z = (k & 4) ? b.mx[2] : b.mn[2];
        const float ex = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
        const float ey = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
        const float ez = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
        float cx, cy, cw;
        if (c.ortho) { cx = p[0] * ex + p[1]; cy = p[2] * ey + p[3]; cw = 1.0f; }
        else { cx = p[0] * ex + p[1] * ez; cy = p[2] * ey + p[3] * ez; cw = -ez; }
        if (!(cw > 1e-4f)) return 1.0f;
        const float nx = cx / cw, ny = cy / cw;
        if (!std::isfinite(nx) || !std::isfinite(ny)) return 1.0f;
        x0 = std::min(x0, nx); x1 = std::max(x1, nx);
        y0 = std::min(y0, ny); y1 = std::max(y1, ny);
    }
    x0 = std::max(x0, -1.0f); y0 = std::max(y0, -1.0f);
    x1 = std::min(x1, 1.0f); y1 = std::min(y1, 1.0f);
    if (!(x1 > x0 && y1 > y0)) return 0.0f;
    return (x1 - x0) * (y1 - y0) * 0.25f;
}
// Before PixDrop of a static run (rigid ones: with per-vertex matrices the rows differ per vertex: not known, 1.00 in the census).
void PixCoverStatic() {
    if ((!PixCensusOn() && PixBlendOffMode() == 0) || !g_cha.pend || !g_cha.pendN || g.vcd[0] != 0) return;
    const uint32_t pi = g.matIdxA & 0x3F;
    const float* m[3];
    for (uint32_t r = 0; r < 3; ++r) m[r] = g.xfPos[pi + r < 64 ? pi + r : 63];
    ClipNow c;
    ClipNowOf(c);
    g_pixFrac = BoxScreenFrac(c, m, g_cha.pend[0]);
}
// GXV_STATICCULL_PAL: every row's box (boxes 1..n) outside one and the same plane, each through its own rows.
bool StaticPalOutside(const RowBox* pb, uint32_t pn) {
    if (pn < 2) return false;
    ClipNow c;
    bool have = false;
    uint32_t all = 31u;
    for (uint32_t i = 1; i < pn && all; ++i) {
        const uint32_t row = pb[i].row;
        if (row > 61) return false; // (the shader clamps there: another matrix)
        if (!have) { ClipNowOf(c); have = true; }
        const float* m[3] = {g.xfPos[row], g.xfPos[row + 1], g.xfPos[row + 2]};
        all &= BoxOutMask(c, m, pb[i]);
    }
    return all != 0;
}
// ================================================================ clusters of a static run (GXV_CLUSTERCULL, vertex25)
// GXV_STATICCULL leaves out a static run only when its whole box is outside one plane. A stage's shape (the cave's shell, a planet's ground)
// is one run whose box covers the view whatever part of it is in front of the camera, so it is drawn whole: the vertex shader runs, and the
// tiler clips, every triangle behind the player. Each big rigid triangle run of a promoted list also has the boxes of its consecutive triangles
// (vgx_dlcache.cpp BuildClusters: about 64 triangles a cluster, in the order the run draws them). Here a cluster whose box is outside one plane of
// the clip volume (BoxOutMask: the very test, margins and side-plane rules of the whole-run cull, through the position matrix the shader multiplies
// by) has all its triangles clipped away whole, so the picture is the same bit for bit; the run is drawn as the indices that remain.
//   * Cost model: a draw costs the GPU's tiler about 17 us (ta23's fit) and the front end 25-40 us; an index about 8-12 ns. So only a stretch of
//     culled clusters that does not add a draw is cheap: the head and the tail of the run (they only shorten the one draw; GXV_CLUSTERCULL_HEAD
//     indices at least, default 384). A gap in the middle splits the run in two draws and is left out only when it holds at least
//     GXV_CLUSTERCULL_GAP indices (default 3000, about the cost of the draw it adds); at most 4 draws per run (the largest gaps).
//   * Not applied: the first use of a list (as GXV_STATICCULL), a run whose indices were replaced by GXV_DLVCACHE's reordered block, an occluder
//     under GXV_OCCLCULL, learned copies and instance buffers (their jobs are joined into palette runs), the warm-up scope, a run without
//     clusters (small, or with per-vertex matrices), the two-core front end's second instance (it draws the whole run).
//   * Seamless: the test runs on every use of the run, so a cluster that comes into the volume is drawn in the picture it enters (the margin of
//     the whole-run test is kept: 4 EFB pixels + the float error bound); nothing is allocated, nothing waits.
//   GXV_CLUSTERCULL: 0 off (the code as before), 1 on, 2 dry (counts what 1 would leave out, draws every run whole), 3 on + checks (build: every
//   vertex of every triangle against its cluster's box, "box bad" must be 0; draw: the ranges tile the run, "range bad" must be 0).
//   GXV_CLUSTERCULL_TRIS (64): triangles a cluster; GXV_CLUSTERCULL_MINRUN (1200): smallest run in indices that gets clusters.
//   The "vtx25:" basic-log line every 10 s counts, a picture: static triangle runs and indices reaching the stage, those with clusters, the
//   indices in culled clusters (all of them, "potential"), what the thresholds leave out, the draws it adds, runs with nothing in view.
struct ClRange { uint32_t s, n; };
struct ClCtr {
    uint64_t pics = 0, runs = 0, idx = 0, noCl = 0, noClIdx = 0, withCl = 0, withClIdx = 0, inside = 0, clTested = 0, clOut = 0, potIdx = 0, tookIdx = 0, tookRuns = 0,
             headTail = 0, gaps = 0, extra = 0, none = 0, noneIdx = 0, rangeBad = 0, firstUse = 0;
    uint64_t bRuns = 0, bCl = 0, bBad = 0;
};
ClCtr g_cl, g_clTot;
const RowBox* g_pendingCl = nullptr;
uint32_t g_pendingClN = 0;
// The ranges of the run's indices that remain (out[0..n)), n 0..4; n 0: no cluster is in view; *took: the indices left out; *pot: the indices in
// culled clusters whatever the thresholds. Fills nothing and returns 1 with the whole run when nothing is left out.
uint32_t ClusterRanges(const RowBox* cl, uint32_t clN, uint32_t idxCount, ClRange out[4], uint32_t* took, uint32_t* pot) {
    static const uint32_t kHead = static_cast<uint32_t>(VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_CLUSTERCULL_HEAD", 384), 3, 1 << 20))));
    static const uint32_t kGap = static_cast<uint32_t>(VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_CLUSTERCULL_GAP", 3000), 3, 1 << 20))));
    *took = 0;
    *pot = 0;
    out[0] = ClRange{0, idxCount};
    if (clN == 0 || clN > 255) return 1;
    const uint32_t pi = g.matIdxA & 0x3F;
    const float* m[3];
    for (uint32_t r = 0; r < 3; ++r) m[r] = g.xfPos[pi + r < 64 ? pi + r : 63]; // (BuildMtxC's rows: StaticBoxOutside)
    ClipNow c;
    ClipNowOf(c);
    bool vis[256];
    uint32_t f = clN, l = 0, nOut = 0;
    for (uint32_t i = 0; i < clN; ++i) {
        vis[i] = BoxOutMask(c, m, cl[i]) == 0;
        if (vis[i]) { if (f == clN) f = i; l = i; }
        else ++nOut;
    }
    g_cl.clTested += clN;
    g_cl.clOut += nOut;
    if (nOut == 0) { ++g_cl.inside; return 1; }
    auto endOf = [&](uint32_t i) { return i + 1 < clN ? cl[i + 1].row : idxCount; };
    if (f == clN) { // every cluster is outside a plane (not always the same one): nothing of the run is in view
        *pot = idxCount;
        *took = idxCount;
        return 0;
    }
    const uint32_t head = cl[f].row, tail = idxCount - endOf(l);
    uint32_t pot2 = head + tail;
    struct Gap { uint32_t s, n; } gaps[256];
    uint32_t ng = 0;
    for (uint32_t i = f; i < l;) {
        if (vis[i]) { ++i; continue; }
        uint32_t j = i;
        while (j < l && !vis[j]) ++j;
        gaps[ng++] = Gap{cl[i].row, cl[j].row - cl[i].row};
        pot2 += cl[j].row - cl[i].row;
        i = j;
    }
    *pot = pot2;
    // the interior gaps that pay for their extra draw: the largest ones, at most 3
    Gap use[3];
    uint32_t nu = 0;
    for (uint32_t i = 0; i < ng; ++i) {
        if (gaps[i].n < kGap) continue;
        if (nu < 3) { use[nu++] = gaps[i]; continue; }
        uint32_t w = 0;
        for (uint32_t k = 1; k < 3; ++k) if (use[k].n < use[w].n) w = k;
        if (gaps[i].n > use[w].n) use[w] = gaps[i];
    }
    std::sort(use, use + nu, [](const Gap& a, const Gap& b) { return a.s < b.s; });
    const uint32_t s0 = head >= kHead ? head : 0u, e1 = tail >= kHead ? idxCount - tail : idxCount;
    uint32_t n = 0, at = s0;
    for (uint32_t k = 0; k < nu; ++k) {
        if (use[k].s <= at || use[k].s + use[k].n > e1) continue;
        out[n++] = ClRange{at, use[k].s - at};
        at = use[k].s + use[k].n;
    }
    out[n++] = ClRange{at, e1 - at};
    uint32_t kept = 0;
    for (uint32_t k = 0; k < n; ++k) kept += out[k].n;
    *took = idxCount - kept;
    if (n == 1 && out[0].s == 0 && out[0].n == idxCount) { *took = 0; return 1; }
    g_cl.headTail += (s0 != 0 ? 1u : 0u) + (e1 != idxCount ? 1u : 0u);
    g_cl.gaps += n - 1;
    return n;
}
void ClusterLog() {
    ++g_cl.pics;
    if (ClusterMode() == 0) return;
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::seconds(10)) return;
    const double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    const ClCtr& k = g_cl;
    const double n = k.pics ? static_cast<double>(k.pics) : 1.0;
    char line[900];
    std::snprintf(line, sizeof line,
                  "vtx25: GXV_CLUSTERCULL=%d, %.0f s, %llu pictures; a picture: static triangle runs reaching the stage %.1f (%.1fk indices): without clusters %.1f (%.1fk), with %.1f (%.1fk); "
                  "clusters tested %.1f, outside a plane %.1f (%.1f runs wholly in view); indices in culled clusters %.1fk (%.1f%% of the clustered runs), left out by the thresholds %.1fk "
                  "in %.1f runs (%.1f head/tail trims, %.1f interior gaps = extra draws), runs with no cluster in view %.1f (%.1fk); first uses %.1f; range bad %llu; since the start: "
                  "%llu runs got clusters (%llu clusters), box bad %llu%s",
                  ClusterMode(), secs, static_cast<unsigned long long>(k.pics), k.runs / n, k.idx / n / 1000.0, k.noCl / n, k.noClIdx / n / 1000.0, k.withCl / n, k.withClIdx / n / 1000.0,
                  k.clTested / n, k.clOut / n, k.inside / n, k.potIdx / n / 1000.0, k.withClIdx ? 100.0 * k.potIdx / static_cast<double>(k.withClIdx) : 0.0, k.tookIdx / n / 1000.0,
                  k.tookRuns / n, k.headTail / n, k.gaps / n, k.none / n, k.noneIdx / n / 1000.0, k.firstUse / n, static_cast<unsigned long long>(k.rangeBad),
                  static_cast<unsigned long long>(g_clTot.bRuns + k.bRuns), static_cast<unsigned long long>(g_clTot.bCl + k.bCl), static_cast<unsigned long long>(g_clTot.bBad + k.bBad),
                  ClusterMode() == 2 ? " (dry: every run drawn whole)" : "");
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    g_clTot.withClIdx += k.withClIdx; g_clTot.potIdx += k.potIdx; g_clTot.tookIdx += k.tookIdx; g_clTot.extra += k.extra; g_clTot.rangeBad += k.rangeBad;
    g_clTot.none += k.none; g_clTot.tookRuns += k.tookRuns;
    g_clTot.bRuns += k.bRuns; g_clTot.bCl += k.bCl; g_clTot.bBad += k.bBad;
    g_cl = ClCtr{};
}
// ================================================================ ta27 census (GXV_TA27; vgx_ta27.h has the reasoning)
// What the GPU's vertex phase is handed, a picture, by the static triangle runs, and what two finer cuts would leave out of the big rigid ones
// (the runs vgx_dlcache.cpp BuildTa27 made sets for): the frustum test of GXV_CLUSTERCULL on clusters of the game's own triangle order ("strip
// order") and on clusters made after a one-time spatial sort ("spatial order"), and the normal-cone test (every triangle of a cluster faces away
// from the eye: the GPU culls them after shading and binning them, so leaving them out of the draw saves the vertex phase, exactly). Reads only:
// nothing is drawn differently. "kept ranges" = the draws a cut would need (each separate stretch of kept clusters).
struct T27Ctr {
    uint64_t pics = 0, runs = 0, idx = 0, vtx = 0, vtxBytes = 0, idxBack = 0, idxNone = 0, idxFront = 0, sets = 0, setIdx = 0, noEye = 0, orthoRuns = 0, firstUse = 0, setIdxCull = 0;
    ta27::Tally o, s;
} g_t27;
uint64_t g_t27Builds = 0, g_t27Clusters = 0, g_t27Bad = 0;
const ta27::RunSet* g_pendingT27 = nullptr;
// One drawn static triangle run (before GXV_DLVCACHE's block replaces its indices): idxCount / vtxCount are the run's own.
void Ta27Use(const ta27::RunSet* st, uint32_t layoutMask, uint32_t vtxCount, uint32_t idxCount, bool firstUse) {
    T27Ctr& c = g_t27;
    ++c.runs;
    c.idx += idxCount;
    c.vtx += vtxCount;
    c.vtxBytes += static_cast<uint64_t>(vtxCount) * MakeLayout(layoutMask).stride;
    const uint8_t cull = cur.rs.cull;
    (cull == CULL_BACK ? c.idxBack : cull == CULL_FRONT ? c.idxFront : c.idxNone) += idxCount;
    if (firstUse) ++c.firstUse;
    if (!st || st->o.empty() || st->s.empty()) return;
    ++c.sets;
    c.setIdx += idxCount;
    const uint32_t pi = g.matIdxA & 0x3F;
    const float* m[3];
    for (uint32_t r = 0; r < 3; ++r) m[r] = g.xfPos[pi + r < 64 ? pi + r : 63];
    ClipNow cn;
    ClipNowOf(cn);
    float E[3] = {0, 0, 0}, fs = 1.0f;
    const bool eye = !cn.ortho && cull != CULL_NONE && ta27::EyeInModel(m, cn.p[0], cn.p[2], E, fs);
    if (cn.ortho) ++c.orthoRuns;
    else if (cull != CULL_NONE && !eye) ++c.noEye;
    if (cull != CULL_NONE) c.setIdxCull += idxCount;
    static std::vector<uint8_t> fo, bk;
    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<ta27::Cl>& cl = pass ? st->s : st->o;
        const uint32_t n = static_cast<uint32_t>(cl.size());
        fo.assign(n + 1u, 0);
        bk.assign(n + 1u, 0);
        for (uint32_t i = 0; i < n; ++i) {
            const ta27::Cl& k = cl[i];
            const RowBox b{{k.mn[0], k.mn[1], k.mn[2]}, {k.mx[0], k.mx[1], k.mx[2]}, k.row, {k.c[0], k.c[1], k.c[2]}, k.r, 0};
            fo[i] = BoxOutMask(cn, m, b) != 0;
            bk[i] = eye && ta27::AllCulled(k, E, fs, cull == CULL_BACK);
        }
        // (Accumulate takes bool arrays; the bytes are 0 / 1)
        static_assert(sizeof(bool) == 1, "bool");
        ta27::Accumulate(cl.data(), n, idxCount, reinterpret_cast<const bool*>(fo.data()), reinterpret_cast<const bool*>(bk.data()), pass ? c.s : c.o);
    }
}
void Ta27Log() {
    ++g_t27.pics;
    if (Ta27Mode() == 0) return;
    static auto last = FeClock::Picture();
    static const long periodMs = std::clamp<long>(EnvInt("GXV_TA27_MS", 10000), 0, 600000); // (tests: 0 = a line every picture)
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::milliseconds(periodMs)) return;
    const double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    const T27Ctr& k = g_t27;
    const double n = k.pics ? static_cast<double>(k.pics) : 1.0;
    const double ci = std::max(1.0, static_cast<double>(k.setIdx));
    char line[1000];
    std::snprintf(line, sizeof line,
                  "ta27: GXV_TA27=%d, %.0f s, %llu pictures; a picture: static triangle runs %.1f (%.1fk indices, %.1fk vertices = %.2f indices a vertex, %.1f KB of vertex data; indices by cull mode back %.1fk none %.1fk front %.1fk); "
                  "runs with sets %.1f (%.1fk indices, %.1fk of them cull-back/front, no eye matrix %.2f, ortho %.2f); of those indices "
                  "[strip order] frustum %.1f%% back-facing %.1f%% either %.1f%% in %.1f clusters-tested, kept ranges %.1f; "
                  "[spatial order] frustum %.1f%% back-facing %.1f%% either %.1f%% kept ranges %.1f; first uses %.1f; built %llu runs (%llu clusters), checks bad %llu",
                  Ta27Mode(), secs, static_cast<unsigned long long>(k.pics), k.runs / n, k.idx / n / 1000.0, k.vtx / n / 1000.0, k.vtx ? static_cast<double>(k.idx) / static_cast<double>(k.vtx) : 0.0,
                  k.vtxBytes / n / 1024.0, k.idxBack / n / 1000.0, k.idxNone / n / 1000.0, k.idxFront / n / 1000.0, k.sets / n, k.setIdx / n / 1000.0, k.setIdxCull / n / 1000.0, k.noEye / n, k.orthoRuns / n,
                  100.0 * k.o.idxOut / ci, 100.0 * k.o.idxBack / ci, 100.0 * k.o.idxUnion / ci, k.o.tested / n, k.o.ranges / n,
                  100.0 * k.s.idxOut / ci, 100.0 * k.s.idxBack / ci, 100.0 * k.s.idxUnion / ci, k.s.ranges / n, k.firstUse / n,
                  static_cast<unsigned long long>(g_t27Builds), static_cast<unsigned long long>(g_t27Clusters), static_cast<unsigned long long>(g_t27Bad));
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    g_t27 = T27Ctr{};
}
// GXV_TINYRUN (dread25; default 0; stage-settable: tiny_run / tiny_px): 0 off, 1 a rigid static run whose box projects to under GXV_TINYRUN_PX pixels squared
// (default 2.0, in pixels of the GX viewport, vgx_tinyrun.h) is not drawn, 2 only the census (the "tinyrun25:" line every 10 s: runs and indices by size class,
// what 1 would leave out; nothing is changed). It runs after the plain cull has decided the run is not outside the view, so the plain cull's counts do not move;
// the first use of a list is never culled (as the plain cull). The run is left out before its state is resolved, exactly as the plain cull leaves it out.
// Visible effect: a far object that covers fewer than ~2 x 1 GX pixels (3 x 1.5 screen pixels at 960x544) as a whole is missing; it would have been a speck.
int TinyRunMode() { return static_cast<int>(PORT_STAGE_KNOB(int32_t, static_cast<int>(std::clamp<long>(port::stageknob::Int("GXV_TINYRUN", 0), 0, 2)))); }
float TinyRunPx() {
    return PORT_STAGE_KNOB(float, std::clamp(port::stageknob::Num("GXV_TINYRUN_PX", 2.0f), 0.0f, 64.0f));
}
struct TinyCtr {
    uint64_t pictures = 0, runs = 0, unknown = 0, culled = 0, culledIdx = 0, culledVtx = 0;
    uint64_t bucketRuns[tinyrun::kBuckets] = {}, bucketIdx[tinyrun::kBuckets] = {};
} g_tiny;
// True when the rigid run (box b through the current position matrix and projection) is to be left out.
bool TinyRunTest(const RowBox& b, uint32_t vtxCount, uint32_t idxCount, int mode) {
    const uint32_t pi = g.matIdxA & 0x3F;
    const float* m[3];
    for (uint32_t r = 0; r < 3; ++r) m[r] = g.xfPos[pi + r < 64 ? pi + r : 63];
    float p[6];
    for (int i = 0; i < 6; ++i) p[i] = u2f(g.xfReg[0x20 + i]);
    const bool ortho = (g.xfReg[0x26] & 1) != 0;
    const float vpW = 2.0f * u2f(g.xfReg[0x1A]), vpH = -2.0f * u2f(g.xfReg[0x1B]);
    const float area = tinyrun::ProjectedAreaPx(p, ortho, m, b.mn, b.mx, vpW, vpH);
    ++g_tiny.runs;
    const int bk = tinyrun::Bucket(area);
    if (bk < 0) ++g_tiny.unknown;
    else { ++g_tiny.bucketRuns[bk]; g_tiny.bucketIdx[bk] += idxCount; }
    if (!tinyrun::IsTiny(area, TinyRunPx())) return false;
    ++g_tiny.culled;
    g_tiny.culledIdx += idxCount;
    g_tiny.culledVtx += vtxCount;
    return mode == 1;
}
void TinyRunLog() {
    ++g_tiny.pictures;
    const int mode = TinyRunMode();
    if (mode == 0) return;
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::seconds(10)) return;
    const double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    const TinyCtr& t = g_tiny;
    const double n = t.pictures ? static_cast<double>(t.pictures) : 1.0;
    char bk[420];
    int at = 0;
    bk[0] = 0;
    static const char* const kName[tinyrun::kBuckets] = {"<1", "<2", "<4", "<8", "<16", "<32", "<64", ">=64"};
    for (int i = 0; i < tinyrun::kBuckets && at < static_cast<int>(sizeof bk) - 48; ++i)
        at += std::snprintf(bk + at, sizeof bk - at, "%s%s px2: %.1f runs %.1fk idx", i ? ", " : "", kName[i], t.bucketRuns[i] / n, t.bucketIdx[i] / n / 1000.0);
    if (port_log_summary)
        port_log_summary("tinyrun25: GXV_TINYRUN=%d (limit %.1f px2), %.0f s, %llu pictures; a picture: rigid static runs measured %.1f (not known %.1f): %s; under the limit %.1f runs, "
                         "%.1fk indices, %.1fk vertices%s",
                         mode, static_cast<double>(TinyRunPx()), secs, static_cast<unsigned long long>(t.pictures), t.runs / n, t.unknown / n, bk, t.culled / n, t.culledIdx / n / 1000.0,
                         t.culledVtx / n / 1000.0, mode == 2 ? " (census: every run drawn)" : " (left out)");
    g_tiny = TinyCtr{};
}
// EmitStaticRun's first test: true when the run is not drawn.
inline bool StaticRunCulled(uint8_t prim, uint32_t vtxCount, uint32_t idxCount) {
    if (!StaticCullEnabled() || prim != PRIM_TRIANGLES || !g_cha.pend || !g_cha.pendN) return false;
    const bool pal = g.vcd[0] != 0;
    if (pal && !StaticCullPalEnabled()) return false;
    // (the first use of a list after it was built is drawn whatever its place: its shaders are linked and its variant goes on the
    // warm-up list when the list first appears, at the area's entry as before, not when it first comes into view. The boxes are
    // the entry's own, Entry::rowBoxes: the flag is cleared when the list is built again.)
    RowBox& all = const_cast<RowBox&>(g_cha.pend[0]);
    if (!all.drawn) {
        // (GXV_FE_TWOCORE: the flag is the display-list cache entry's, which the second worker reads; its own first uses note it for the player)
        two::AGuard l;
        two::Bump(VGX2_EP_DL);
        all.drawn = 1;
        return false;
    }
    if (pal) {
        ++g_cc.palRuns;
        if (!StaticPalOutside(g_cha.pend, g_cha.pendN)) return false;
        ++g_cc.palCulled;
        g_cc.palIdx += idxCount;
    } else {
        ++g_scRuns;
        ++g_cc.runs;
        if (!StaticBoxOutside(g_cha.pend[0])) {
            if (const int tm = TinyRunMode(); tm != 0 && TinyRunTest(g_cha.pend[0], vtxCount, idxCount, tm)) goto tiny_out; // (dread25)
            return false;
        }
        ++g_scCulled;
        ++g_cc.culled;
        g_cc.idx += idxCount;
        g_scCulledVerts += vtxCount;
    }
tiny_out: // (dread25: GXV_TINYRUN leaves the run out the way the plain cull does)
    // (what the draw's ResolveState and CharaNote would have consumed)
    g_pendingPosMtxMax = 0xFF;
    g_cha.pend = nullptr;
    g_cha.pendN = 0;
    return true;
}
// GXV_DYNCULL (after ResolveState): whether every decoded vertex v[0..count) lies outside one plane of the clip volume.
// GXV_HOTPROF_DYNCULL (hotprof25; from the console's execution-count profile of 11 Nov: GXV_DYNCULL's test is the front end's largest single
// block: 9.16 M draws tested in 18,618 pictures, 5.93 M of them left out (65 %: particle quads off screen), 47.4 M points through
// PointOutMask, i.e. ~2,500 points a picture each paying three row sums, ~20 fabs and the margin). The same answer for every draw, cheaper:
//   * a point that is beyond none of the planes `all` still holds, at margin 0, ends the test: its mask can have none of those bits (the
//     margin is positive when finite, and a mask is 0 when it is not), so `all` becomes 0: "not outside" (fe24's early-out, for every
//     point, and only against the planes still in question);
//   * the absolute values of the matrix rows are taken once a draw, not once a point (the same numbers);
//   * the margin is first bounded from above: the row sums it is made of are at most R * max(1, |x|, |y|, |z|), R the largest absolute row
//     sum of the matrix (once a draw), times 1.00001 for the rounding of both sides (their relative errors are under 1e-6). A plane the
//     point is beyond by more than the bound is beyond the margin too; only when a plane in question is between the margin's bound and 0
//     is the exact margin (the three row sums) computed, and then the decision is the exact one. So every decision is PointOutMask's.
// Rigid draws in a perspective projection (particles, small shapes); per-vertex matrices and orthographic projections take the loop as
// before. 0: the code as before. 1 (default): on. 2: verify (both ways for every draw, the old answer used, mismatches counted).
int HotprofDynCull() {
    const int m = VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_HOTPROF_DYNCULL", 1), 0, 2)));
    return m;
}
// GXV_HOTPROF_BOXCULL (hotprof25 round 2; exact; vgx_hotprof_boxcull.inc has the reasons): BoxOutMask, the box test of GXV_STATICCULL and
// the cluster cull (83 boxes a picture reach its eight corners in the console profile), ends as soon as no plane is left that every
// corner so far is beyond; the mask is the same. 0: the test as before (default until a console verify). 1: on. 2: verify (both for
// every box, the old mask used, mismatches counted: the "hotprof: GXV_HOTPROF_BOXCULL=2" line every 10 s).
int HotprofBoxCull() {
    const int m = VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_HOTPROF_BOXCULL", 0), 0, 2)));
    return m;
}
uint64_t g_hpDynDraws, g_hpDynBad, g_hpDynOut;
#include "vgx_hotprof_dyncull.inc" // (HpRigidOutside: the rigid, perspective case; a file of its own so that tests/vgx_hotprof_dyncull.cpp runs the very code)
bool DynDrawOutsideFast(const uint8_t* v, uint32_t count, uint32_t stride) {
    const bool perVtx = cur.vs.perVtxPos != 0;
    uint32_t maxRow = 61;
    if (perVtx) maxRow = cur.vs.perVtxPos == 3 ? static_cast<uint32_t>(kPalSmallMaxRow) : (cur.vs.perVtxPos == 2 ? static_cast<uint32_t>(kPalShortMaxRow) : 61u);
    const float* m[3];
    if (!perVtx) {
        const uint32_t pi = g.matIdxA & 0x3F;
        if (pi > 61) return false; // (BuildMtxC and the CPU transform read different rows there)
        m[0] = g.xfPos[pi]; m[1] = g.xfPos[pi + 1]; m[2] = g.xfPos[pi + 2];
    }
    ClipNow c;
    ClipNowOf(c);
    uint32_t all = 31u;
    if (perVtx || c.ortho) {
        // (per-vertex matrices, an orthographic projection: the loop as before)
        for (uint32_t i = 0; i < count && all; ++i, v += stride) {
            float q[4];
            std::memcpy(q, v, sizeof q);
            if (perVtx) {
                if (!(q[3] >= 0.0f && q[3] < 64.0f)) return false;
                const uint32_t row = static_cast<uint32_t>(q[3] + 0.5f); // (the shader's int(a_pos.w + 0.5))
                if (row > maxRow) return false;                          // (the shader clamps there)
                m[0] = g.xfPos[row]; m[1] = g.xfPos[row + 1]; m[2] = g.xfPos[row + 2];
            }
            all &= PointOutMask(c, m, q[0], q[1], q[2]);
        }
        return all != 0;
    }
    return HpRigidOutside(c, m, v, count, stride); // (rigid, perspective: vgx_hotprof_dyncull.inc)
}
bool DynDrawOutsideOld(const uint8_t* v, uint32_t count, uint32_t stride);
bool DynDrawOutside(const uint8_t* v, uint32_t count, uint32_t stride) {
    const int hp = HotprofDynCull();
    if (hp == 1) return DynDrawOutsideFast(v, count, stride);
    if (hp == 2) {
        const bool o = DynDrawOutsideOld(v, count, stride), f = DynDrawOutsideFast(v, count, stride);
        ++g_hpDynDraws;
        if (o) ++g_hpDynOut;
        if (o != f) ++g_hpDynBad;
        return o;
    }
    return DynDrawOutsideOld(v, count, stride);
}
bool DynDrawOutsideOld(const uint8_t* v, uint32_t count, uint32_t stride) {
    const bool perVtx = cur.vs.perVtxPos != 0;
    uint32_t maxRow = 61;
    if (perVtx) maxRow = cur.vs.perVtxPos == 3 ? static_cast<uint32_t>(kPalSmallMaxRow) : (cur.vs.perVtxPos == 2 ? static_cast<uint32_t>(kPalShortMaxRow) : 61u);
    const float* m[3];
    if (!perVtx) {
        const uint32_t pi = g.matIdxA & 0x3F;
        if (pi > 61) return false; // (BuildMtxC and the CPU transform read different rows there)
        m[0] = g.xfPos[pi]; m[1] = g.xfPos[pi + 1]; m[2] = g.xfPos[pi + 2];
    }
    ClipNow c;
    ClipNowOf(c);
    uint32_t all = 31u;
    for (uint32_t i = 0; i < count && all; ++i, v += stride) {
        float q[4];
        std::memcpy(q, v, sizeof q);
        if (perVtx) {
            if (!(q[3] >= 0.0f && q[3] < 64.0f)) return false;
            const uint32_t row = static_cast<uint32_t>(q[3] + 0.5f); // (the shader's int(a_pos.w + 0.5))
            if (row > maxRow) return false;                          // (the shader clamps there)
            m[0] = g.xfPos[row]; m[1] = g.xfPos[row + 1]; m[2] = g.xfPos[row + 2];
        }
        all &= PointOutMask(c, m, q[0], q[1], q[2]);
    }
    return all != 0;
}
// GXV_DYN_CENSUS=1: per-class counts and front-end time of every dynamic (CPU-decoded) draw; one "dyn-census:" line every 10 s.
// Class = primitive x vertex-count bucket. Reads only; =0 is the code as before.
namespace {
struct DynCen {
    uint64_t n[3][6] = {}, v[3][6] = {}, ns[3][6] = {}, soft[3][6] = {}, pics = 0;
    uint64_t on[2] = {}, ov[2] = {}, ons[2] = {}; // (cpu34d: by origin: [0] decoded from the stream, [1] a cached list's segment)
};
DynCen g_dynCen;
// (streamc31: GXV_DYN_CENSUS is a set of bits: 1 this "dyn-census:" line (and EmitDraw's scope), 2 the stream-decode census, "dyn-census decode:"
// lines (vgx_deccen.h, vgx_state.cpp DecCenNote); 3 both)
inline int DynCenMode() { static const int m = static_cast<int>(EnvInt("GXV_DYN_CENSUS", 0)); return m; }
inline bool DynCenOn() { return (DynCenMode() & 1) != 0; }
inline int DynBucket(uint32_t c) { return c <= 4 ? 0 : c <= 8 ? 1 : c <= 16 ? 2 : c <= 64 ? 3 : c <= 512 ? 4 : 5; }
struct DynCenScope {
    int pr, b; uint32_t c; FeClock::Time t0; bool on;
    DynCenScope(uint8_t prim, uint32_t count) : pr(prim < 3 ? prim : 0), b(DynBucket(count)), c(count), on(DynCenOn()) {
        if (on) t0 = FeClock::Read();
    }
    ~DynCenScope() {
        if (!on) return;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(FeClock::Read() - t0).count();
        g_dynCen.n[pr][b]++; g_dynCen.v[pr][b] += c; g_dynCen.ns[pr][b] += static_cast<uint64_t>(ns);
        const int og = g_dynCenList != 0 ? 1 : 0;
        g_dynCen.on[og]++; g_dynCen.ov[og] += c; g_dynCen.ons[og] += static_cast<uint64_t>(ns);
        if (cur.soft) g_dynCen.soft[pr][b]++;
    }
};
void DynCenLog() {
    const int mode = DynCenMode();
    if (!(mode & 3)) return;
    if (mode & 2) DecCenPicture(); // (the stream-decode census: this picture's draws become the ones the next picture's are compared with)
    ++g_dynCen.pics;
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::milliseconds(EnvInt("GXV_DYN_CENSUS_MS", 10000))) return;
    last = now;
    if (mode & 2)
        DecCenLog([](const char* l) {
            if (port_log_summary) port_log_summary("%s", l);
            else std::fprintf(stderr, "[vgx] %s\n", l);
        });
    if (!(mode & 1)) { g_dynCen = DynCen{}; return; }
    static const char* pn[3] = {"tri", "line", "point"};
    static const char* bn[6] = {"<=4", "<=8", "<=16", "<=64", "<=512", ">512"};
    const double pc = static_cast<double>(g_dynCen.pics);
    char line[1200]; int o = std::snprintf(line, sizeof line, "dyn-census: %llu pictures; per picture [draws/vertices/fe_us/soft]:", static_cast<unsigned long long>(g_dynCen.pics));
    double td = 0, tus = 0;
    for (int p = 0; p < 3; ++p)
        for (int b = 0; b < 6; ++b) {
            if (!g_dynCen.n[p][b]) continue;
            td += g_dynCen.n[p][b] / pc; tus += g_dynCen.ns[p][b] / 1000.0 / pc;
            o += std::snprintf(line + o, sizeof line - o, " %s%s=%.1f/%.0f/%.0f/%.1f", pn[p], bn[b], g_dynCen.n[p][b] / pc, g_dynCen.v[p][b] / pc, g_dynCen.ns[p][b] / 1000.0 / pc, g_dynCen.soft[p][b] / pc);
            if (o > 1100) break;
        }
    o += std::snprintf(line + o, sizeof line - o, " | total %.1f draws, %.0f us (clock reads included, about 0.1 us each)", td, tus);
    // (cpu34d: where they come from. Particle quads are among the stream's: the "ptcl-cull:" line counts those.)
    if (o < 1000)
        std::snprintf(line + o, sizeof line - o, " | by origin [draws/vertices/fe_us]: cached small lists (J3D shapes) %.1f/%.0f/%.0f, decoded from the stream (particles, 2D, immediate) %.1f/%.0f/%.0f",
                      g_dynCen.on[1] / pc, g_dynCen.ov[1] / pc, g_dynCen.ons[1] / 1000.0 / pc, g_dynCen.on[0] / pc, g_dynCen.ov[0] / pc, g_dynCen.ons[0] / 1000.0 / pc);
    g_dynCen = DynCen{};
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
}
} // namespace
// GXV_FE24_*: the engagement line, every 10 s: a picture's counts.
void Fe24Log() {
    static uint64_t pics, h0, m0, e0, f0;
    ++pics;
    if (HotprofDynCull() == 2) { // (GXV_HOTPROF_DYNCULL=2: the verify line, every 10 s)
        static auto hpLast = FeClock::Picture();
        const auto hpNow = FeClock::Picture();
        if (hpNow - hpLast >= std::chrono::milliseconds(EnvInt("GXV_FE24_LOG_MS", 10000))) {
            hpLast = hpNow;
            char hl[200];
            std::snprintf(hl, sizeof hl, "hotprof: GXV_HOTPROF_DYNCULL=2: %llu dynamic draws tested both ways since the start (%llu outside), %llu mismatches",
                          static_cast<unsigned long long>(g_hpDynDraws), static_cast<unsigned long long>(g_hpDynOut), static_cast<unsigned long long>(g_hpDynBad));
            if (port_log_summary) port_log_summary("%s", hl);
            else std::fprintf(stderr, "[vgx] %s\n", hl);
        }
    }
    if (HotprofBoxCull() == 2) { // (GXV_HOTPROF_BOXCULL=2: the verify line, every 10 s)
        static auto hbLast = FeClock::Picture();
        const auto hbNow = FeClock::Picture();
        if (hbNow - hbLast >= std::chrono::milliseconds(EnvInt("GXV_FE24_LOG_MS", 10000))) {
            hbLast = hbNow;
            char hl[200];
            std::snprintf(hl, sizeof hl, "hotprof: GXV_HOTPROF_BOXCULL=2: %llu boxes tested both ways since the start (%llu outside), %llu mismatches",
                          static_cast<unsigned long long>(g_hpBoxCalls), static_cast<unsigned long long>(g_hpBoxOut), static_cast<unsigned long long>(g_hpBoxBad));
            if (port_log_summary) port_log_summary("%s", hl);
            else std::fprintf(stderr, "[vgx] %s\n", hl);
        }
    }
    const int mode = VGX_KNOB(EnvInt("GXV_FE24_CLIP", 0));
    if (mode == 0) return;
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::milliseconds(EnvInt("GXV_FE24_LOG_MS", 10000))) return;
    last = now;
    const double n = static_cast<double>(pics);
    char line[384];
    std::snprintf(line, sizeof line,
                  "fe24: GXV_FE24_CLIP=%d, %llu pictures; a picture: clip volume memo hits %.1f, recomputed %.1f (mismatches %llu), points inside without the margin %.1f, with it %.1f",
                  mode, static_cast<unsigned long long>(pics), (g_fe24ClipHit - h0) / n, (g_fe24ClipMiss - m0) / n, static_cast<unsigned long long>(g_fe24ClipBad),
                  (g_fe24PtEarly - e0) / n, (g_fe24PtFull - f0) / n);
    pics = 0; h0 = g_fe24ClipHit; m0 = g_fe24ClipMiss; e0 = g_fe24PtEarly; f0 = g_fe24PtFull;
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
}
// GXV_CACHE24: the engagement line, every 10 s: a picture's hints and how good the predictions were.
void Cache24Log() {
    static uint64_t pics, pf0, rc0, rr0, ta0, ts0, tg0, be0;
    ++pics;
    const int mode = Cache24();
    if (mode == 0) return;
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::milliseconds(EnvInt("GXV_CACHE24_LOG_MS", 10000))) return;
    last = now;
    const double n = static_cast<double>(pics);
    const C24Stats& s = g_c24s;
    const uint64_t be = backend::Cache24Hints();
    char line[400];
    std::snprintf(line, sizeof line,
                  "cache24: GXV_CACHE24=%d (prefetches %s) dist %u, %llu pictures; a picture: front-end hints %.1f, back-end hints %.1f; retained calls with a predecessor %.1f, successor predicted right %.1f; "
                  "trace look-aheads %.1f (state-list records %.1f, geometry %.1f)",
                  mode, (mode & 1) ? ((mode & 2) ? "on, with state-list stages" : "on") : "off", Cache24Dist(), static_cast<unsigned long long>(pics), (g_c24Pf - pf0) / n, (be - be0) / n, (s.rtCalls - rc0) / n,
                  (s.rtRight - rr0) / n, (s.trAhead - ta0) / n, (s.trSl - ts0) / n, (s.trGeo - tg0) / n);
    pics = 0; pf0 = g_c24Pf; rc0 = s.rtCalls; rr0 = s.rtRight; ta0 = s.trAhead; ts0 = s.trSl; tg0 = s.trGeo; be0 = be;
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
}
void CullLog() {
    ++g_cc.pictures;
    if (!StaticCullEnabled() && !DynCullMin() && !g_cc.vcHad) return;
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::seconds(10)) return;
    const double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    const CullCtr& k = g_cc;
    const double n = k.pictures ? static_cast<double>(k.pictures) : 1.0;
    char line[768];
    uint64_t vl[6] = {};
    DlVcacheCounters(vl);
    int len = std::snprintf(line, sizeof line,
                  "ta23: %.0f s, %llu pictures; a picture: static runs culled %.1f of %.1f (%.1fk indices); GXV_STATICCULL_PAL=%d: per-vertex-matrix "
                  "runs culled %.1f of %.1f (%.1fk indices); GXV_DYNCULL=%u: dynamic draws culled %.1f of %.1f tested (%.1fk indices), %.1f not decoded yet; "
                  "GXV_DLVCACHE: %.1f of %.1f static runs with a reordered block drew it (%.1fk of %.1fk indices)",
                  secs, static_cast<unsigned long long>(k.pictures), k.culled / n, k.runs / n, k.idx / n / 1000.0, StaticCullPalEnabled() ? 1 : 0,
                  k.palCulled / n, k.palRuns / n, k.palIdx / n / 1000.0, DynCullMin(), k.dynCulled / n, k.dynTested / n, k.dynIdx / n / 1000.0, k.dynLater / n,
                  k.vcTook / n, k.vcHad / n, k.vcIdxTook / n / 1000.0, k.vcIdxHad / n / 1000.0);
    if (vl[0] && len > 0 && len < static_cast<int>(sizeof line))
        std::snprintf(line + len, sizeof line - len,
                      "; since the start %llu runs reordered (%llu triangles, %llu left as they were by the budget), modelled vertex-shader runs of "
                      "those: unwelded %llu, welded %llu, reordered %llu",
                      static_cast<unsigned long long>(vl[0]), static_cast<unsigned long long>(vl[1]), static_cast<unsigned long long>(vl[5]),
                      static_cast<unsigned long long>(vl[2]), static_cast<unsigned long long>(vl[3]), static_cast<unsigned long long>(vl[4]));
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    g_ccTotal.runs += k.runs; g_ccTotal.culled += k.culled; g_ccTotal.idx += k.idx; g_ccTotal.palRuns += k.palRuns; g_ccTotal.palCulled += k.palCulled;
    g_ccTotal.palIdx += k.palIdx; g_ccTotal.dynTested += k.dynTested; g_ccTotal.dynCulled += k.dynCulled; g_ccTotal.dynIdx += k.dynIdx;
    g_ccTotal.dynLater += k.dynLater; g_ccTotal.pictures += k.pictures;
    g_ccTotal.vcHad += k.vcHad; g_ccTotal.vcTook += k.vcTook; g_ccTotal.vcIdxHad += k.vcIdxHad; g_ccTotal.vcIdxTook += k.vcIdxTook;
    g_cc = CullCtr{};
}
} // namespace
void StaticCullCounters(uint64_t out[3]) { out[0] = g_scRuns; out[1] = g_scCulled; out[2] = g_scCulledVerts; }
// GXV_CLUSTERCULL (read once, default 0; see "clusters of a static run"), its limits and the build's counters (vgx_dlcache.cpp BuildClusters)
int ClusterMode() { return static_cast<int>(VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_CLUSTERCULL", 0), 0, 3)))); }
uint32_t ClusterTris() { static const uint32_t t = static_cast<uint32_t>(std::clamp<long>(EnvInt("GXV_CLUSTERCULL_TRIS", 64), 8, 4096)); return t; }
uint32_t ClusterMinRun() { static const uint32_t t = static_cast<uint32_t>(std::clamp<long>(EnvInt("GXV_CLUSTERCULL_MINRUN", 1200), 96, 1 << 20)); return t; }
void ClusterBuildNote(uint32_t runs, uint32_t clusters, uint32_t boxBad) { g_cl.bRuns += runs; g_cl.bCl += clusters; g_cl.bBad += boxBad; }
void SetPendingClusters(const RowBox* cl, uint32_t n) { g_pendingCl = cl; g_pendingClN = n; }
// GXV_TA27 (see "ta27 census")
int Ta27Mode() { static const int m = static_cast<int>(std::clamp<long>(EnvInt("GXV_TA27", 0), 0, 2)); return m; }
uint32_t Ta27Tris() { static const uint32_t t = static_cast<uint32_t>(std::clamp<long>(EnvInt("GXV_TA27_TRIS", 64), 8, 4096)); return t; }
uint32_t Ta27MinRun() { static const uint32_t t = static_cast<uint32_t>(std::clamp<long>(EnvInt("GXV_TA27_MINRUN", 600), 96, 1 << 20)); return t; }
void SetPendingTa27(const ta27::RunSet* st) { g_pendingT27 = st; }
void Ta27BuildNote(uint32_t runs, uint32_t clusters, uint32_t bad) { g_t27Builds += runs; g_t27Clusters += clusters; g_t27Bad += bad; }
// (tests) totals since the start: [0] indices of runs with clusters, [1] indices in culled clusters, [2] indices left out, [3] draws added, [4] range checks failed
// (GXV_CLUSTERCULL=3), [5] box checks failed at the build, [6] runs with no cluster in view, [7] runs that had indices left out
void ClusterCounters(uint64_t out[8]) {
    const ClCtr& a = g_clTot;
    const ClCtr& b = g_cl;
    out[0] = a.withClIdx + b.withClIdx; out[1] = a.potIdx + b.potIdx; out[2] = a.tookIdx + b.tookIdx; out[3] = a.extra + b.extra;
    out[4] = a.rangeBad + b.rangeBad; out[5] = a.bBad + b.bBad; out[6] = a.none + b.none; out[7] = a.tookRuns + b.tookRuns;
}
// (tests) ta23 totals since the start: [0] per-vertex-matrix runs tested, [1] culled, [2] their indices, [3] dynamic draws tested,
// [4] culled, [5] their indices, [6] not decoded yet, [7] static (rigid) culled indices
void CullCounters23(uint64_t out[8]) {
    const CullCtr& a = g_ccTotal;
    const CullCtr& b = g_cc;
    out[0] = a.palRuns + b.palRuns; out[1] = a.palCulled + b.palCulled; out[2] = a.palIdx + b.palIdx;
    out[3] = a.dynTested + b.dynTested; out[4] = a.dynCulled + b.dynCulled; out[5] = a.dynIdx + b.dynIdx; out[6] = a.dynLater + b.dynLater;
    out[7] = a.idx + b.idx;
}
void VcacheDrawCounters(uint64_t out[4]) {
    const CullCtr& a = g_ccTotal;
    const CullCtr& b = g_cc;
    out[0] = a.vcTook + b.vcTook; out[1] = a.vcHad + b.vcHad; out[2] = a.vcIdxTook + b.vcIdxTook; out[3] = a.vcIdxHad + b.vcIdxHad;
}
// ================================================================ view lists (GXV_FACECULL, farlod32, 13 Nov 2026)
// vgx_facecull.h has the method and the proof; vgx_dlcache.cpp "view lists" what is kept per run. Here: the choice at a draw.
//   GXV_FACECULL=0 (default) off: nothing is kept, every run is drawn as before. 1 on. 2 census: the lists are worked out and counted
//   (the "facecull:" line) but nothing is uploaded and every run is drawn whole. 3 test picture: only the triangles a list leaves out
//   are drawn; since the GPU culls every one of them, the runs must vanish from the picture: anything of them that still shows is a fault.
//   GXV_FACECULL_MARGIN (default 6): the camera may move this many percent of its largest distance to the run (at least 50 units)
//   before the list is made again. GXV_FACECULL_BUDGET (default 8000): triangles tested for new lists per picture (the first list of
//   a picture is always allowed; a run over the budget is drawn whole until its turn). GXV_FACECULL_GAIN (default 10): a list that
//   leaves out less than this percent of the run is not kept (the run sleeps 120 pictures, twice as long each time, up to 1920).
//   GXV_FACECULL_MIN / GXV_FACECULL_KB: vgx_dlcache.cpp. GXV_FACECULL_LOG_S (default 10): seconds between "facecull:" lines. All read once.
// A run drawn twice in one picture from places further apart than the margin (instances of one model) keeps its list for the first; the
// others are drawn whole, and after 16 such pictures the run sleeps for 600.
// The "facecull:" line (basic log, every 10 s while the switch is on) has, per picture: the runs with a record, those drawn from a
// list, their indices before and after, the indices saved, the lists made and the triangles tested for them, and why runs were drawn
// whole. Performance is the console's to measure: the saved indices times the fit's 19.3 ns is an upper estimate (a culled triangle
// costs the GPU its vertices and the cull test, not the tiling).
int FaceCullMode() {
    static const int m = static_cast<int>(std::clamp<long>(EnvInt("GXV_FACECULL", 0), 0, 3));
    return m;
}
namespace {
float FaceMarginFrac() {
    static const float f = static_cast<float>(std::clamp<long>(EnvInt("GXV_FACECULL_MARGIN", 6), 1, 50)) * 0.01f;
    return f;
}
uint32_t FaceBudgetTris() {
    static const uint32_t n = static_cast<uint32_t>(std::clamp<long>(EnvInt("GXV_FACECULL_BUDGET", 8000), 0, 1L << 24));
    return n;
}
uint32_t FaceGainPct() {
    static const uint32_t n = static_cast<uint32_t>(std::clamp<long>(EnvInt("GXV_FACECULL_GAIN", 10), 0, 90));
    return n;
}
struct FaceCtr {
    uint64_t pictures = 0, offered = 0, took = 0, nothing = 0, idxFull = 0, idxDrawn = 0, vtxFull = 0, vtxDrawn = 0;
    uint64_t rebuilds = 0, trisTested = 0, noCull = 0, noEye = 0, asleep = 0, overBudget = 0, conflict = 0, tooSoon = 0, noGain = 0;
    // (the second line: where a picture's static triangle indices go. all: every static triangle run that is submitted; without a record:
    // per-vertex matrices, under GXV_FACECULL_MIN, other [no room, a learned copy, an instance buffer, the warm-up]; with one, drawn whole, by reason)
    uint64_t allIdx = 0, allRuns = 0, skinIdx = 0, skinRuns = 0, smallIdx = 0, smallRuns = 0, otherIdx = 0, otherRuns = 0;
    uint64_t noCullIdx = 0, noEyeIdx = 0, sleepGainIdx = 0, sleepGainRuns = 0, sleepInstIdx = 0, sleepInstRuns = 0, conflictIdx = 0, budgetIdx = 0, soonIdx = 0;
} g_fc;
uint64_t g_fcFrame = ~0ull;
uint32_t g_fcTris = 0; // (triangles tested in picture g_fcFrame)
std::vector<uint16_t> g_fcIdx, g_fcRemap;
std::vector<uint8_t> g_fcVtx;
enum FaceResult { kFaceWhole, kFaceList, kFaceNothing };
// The run's view list for this draw, made (again) if need be. kFaceList: *bufId / *idxCount / *vtxCount are the list's.
FaceResult FaceTake(facecull::Run& r, uint32_t idxCount, uint32_t vtxCount, uint32_t* bufId, uint32_t* idxOut, uint32_t* vtxOut) {
    const int mode = FaceCullMode();
    ++g_fc.offered;
    if (cur.rs.cull != CULL_BACK && cur.rs.cull != CULL_FRONT) { ++g_fc.noCull; g_fc.noCullIdx += idxCount; return kFaceWhole; }
    if ((g.xfReg[0x26] & 1) != 0 || g.vcd[0] != 0 || r.idx.size() != idxCount) { ++g_fc.noEye; g_fc.noEyeIdx += idxCount; return kFaceWhole; }
    const uint32_t pi = g.matIdxA & 0x3F;
    const float* m[3];
    for (uint32_t k = 0; k < 3; ++k) m[k] = g.xfPos[pi + k < 64 ? pi + k : 63];
    float eye[3];
    int det = 0;
    if (!facecull::EyeOf(m, eye, &det)) { ++g_fc.noEye; g_fc.noEyeIdx += idxCount; return kFaceWhole; }
    const int k = facecull::Handedness(det, u2f(g.xfReg[0x20]), u2f(g.xfReg[0x22]), u2f(g.xfReg[0x1A]), u2f(g.xfReg[0x1B]));
    if (k == 0) { ++g_fc.noEye; g_fc.noEyeIdx += idxCount; return kFaceWhole; }
    // (back culling: the counter-clockwise triangles, k.f > 0, are the culled ones; front culling: the others)
    const int side = cur.rs.cull == CULL_BACK ? k : -k;
    const uint64_t frame = g_frame;
    if (!facecull::Valid(r, eye, side)) {
        if (r.sleepUntil > frame) {
            ++g_fc.asleep;
            if (r.sleepWhy == 2) { ++g_fc.sleepInstRuns; g_fc.sleepInstIdx += idxCount; }
            else { ++g_fc.sleepGainRuns; g_fc.sleepGainIdx += idxCount; }
            return kFaceWhole;
        }
        if (r.margin > 0.0f && r.usedFrame == frame) {
            // (another use of the run in this picture drew the list from elsewhere: instances)
            ++g_fc.conflict;
            g_fc.conflictIdx += idxCount;
            if (++r.conflicts >= 16) { r.conflicts = 0; r.sleepUntil = frame + 600; r.sleepWhy = 2; DlFaceDrop(r); }
            return kFaceWhole;
        }
        if (r.margin > 0.0f && frame < r.builtFrame + 2) { ++g_fc.tooSoon; g_fc.soonIdx += idxCount; return kFaceWhole; }
        if (frame != g_fcFrame) { g_fcFrame = frame; g_fcTris = 0; }
        const uint32_t tris = idxCount / 3;
        if (g_fcTris != 0 && g_fcTris + tris > FaceBudgetTris()) { ++g_fc.overBudget; g_fc.budgetIdx += idxCount; return kFaceWhole; }
        g_fcTris += tris;
        const float reach = facecull::Reach(r, eye), margin = std::max(50.0f, FaceMarginFrac() * reach);
        const uint32_t kept = facecull::Filter(r, eye, side, facecull::Threshold(margin, reach), mode == 3, g_fcIdx, g_fcVtx, g_fcRemap);
        ++g_fc.rebuilds;
        g_fc.trisTested += tris;
        if (mode != 3 && static_cast<uint64_t>(kept) * 100u > static_cast<uint64_t>(idxCount) * (100u - FaceGainPct())) {
            ++g_fc.noGain;
            r.sleepLen = static_cast<uint16_t>(std::min<uint32_t>(1920u, std::max<uint32_t>(120u, r.sleepLen * 2u)));
            r.sleepUntil = frame + r.sleepLen;
            r.sleepWhy = 1;
            g_fc.sleepGainIdx += idxCount;
            ++g_fc.sleepGainRuns;
            DlFaceDrop(r);
            return kFaceWhole;
        }
        r.sleepLen = 0;
        const uint32_t keptVerts = r.stride ? static_cast<uint32_t>(g_fcVtx.size() / r.stride) : 0u;
        if (mode == 2) g_fcVtx.clear(); // (census: the list's numbers only, no buffer)
        // (the list is drawn while the eye is within 0.9 of the margin its threshold was made for)
        DlFaceInstall(r, g_fcVtx, g_fcIdx, eye, 0.9f * margin, side);
        r.idxKept = kept;
        r.vtxKept = keptVerts;
    }
    r.usedFrame = frame;
    ++g_fc.took;
    g_fc.idxFull += idxCount;
    g_fc.idxDrawn += r.idxKept;
    g_fc.vtxFull += vtxCount;
    g_fc.vtxDrawn += r.vtxKept;
    if (mode == 2) return kFaceWhole;
    if (r.idxKept == 0) { ++g_fc.nothing; return kFaceNothing; }
    if (!r.bufId) return kFaceWhole;
    *bufId = r.bufId;
    *idxOut = r.idxKept;
    *vtxOut = r.vtxKept;
    return kFaceList;
}
}  // namespace
void FaceCullLog() {
    if (FaceCullMode() == 0) return;
    DlFacePump(g_frame);
    ++g_fc.pictures;
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    static const long every = std::clamp<long>(EnvInt("GXV_FACECULL_LOG_S", 10), 0, 600); // (0: every picture, for the host test)
    if (now - last < std::chrono::seconds(every)) return;
    const double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    const FaceCtr& c = g_fc;
    const double n = c.pictures ? static_cast<double>(c.pictures) : 1.0;
    uint64_t t[6];
    DlFaceTotals(t);
    char line[900];
    std::snprintf(line, sizeof line,
                  "facecull: GXV_FACECULL=%d, %.0f s, %llu pictures; a picture: runs with a record %.1f, drawn from a view list %.1f (%.1f of them with nothing "
                  "facing the camera), their indices %.1fk -> %.1fk (saved %.1fk, %.0f%%), vertices %.1fk -> %.1fk; lists made %.2f (%.0f triangles tested); drawn "
                  "whole: no culling %.1f, no eye point %.1f, asleep %.1f, over the budget %.1f, second place in one picture %.1f, too soon %.1f, no gain %.2f; "
                  "now: %llu runs kept (%llu KB), %llu lists (%llu KB of buffers), %llu refused for memory, %llu buffers waiting to be freed%s",
                  FaceCullMode(), secs, static_cast<unsigned long long>(c.pictures), c.offered / n, c.took / n, c.nothing / n, c.idxFull / n / 1000.0, c.idxDrawn / n / 1000.0,
                  (c.idxFull - c.idxDrawn) / n / 1000.0, c.idxFull ? 100.0 * static_cast<double>(c.idxFull - c.idxDrawn) / static_cast<double>(c.idxFull) : 0.0,
                  c.vtxFull / n / 1000.0, c.vtxDrawn / n / 1000.0, c.rebuilds / n, c.trisTested / n, c.noCull / n, c.noEye / n, c.asleep / n, c.overBudget / n, c.conflict / n,
                  c.tooSoon / n, c.noGain / n, static_cast<unsigned long long>(t[0]), static_cast<unsigned long long>(t[1] >> 10), static_cast<unsigned long long>(t[2]),
                  static_cast<unsigned long long>(t[3] >> 10), static_cast<unsigned long long>(t[4]), static_cast<unsigned long long>(t[5]),
                  FaceCullMode() == 2 ? " (census: every run drawn whole)" : FaceCullMode() == 3 ? " (TEST PICTURE: only the left-out triangles are drawn)" : "");
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    // (where the indices go: what an extension could still reach. In the test picture, mode 3, "drawn" above is what is left OUT in mode 1.)
    std::snprintf(line, sizeof line,
                  "facecull: indices, a picture: static triangle runs submitted %.1f (%.1fk indices); no record: per-vertex matrices %.1f (%.1fk), under %u "
                  "indices %.1f (%.1fk), other %.1f (%.1fk); with a record, drawn whole: no culling %.1fk, no eye point %.1fk, asleep after a list without gain "
                  "%.1f (%.1fk), asleep as drawn from several places %.1f (%.1fk), second place in one picture %.1fk, over the budget %.1fk, too soon %.1fk",
                  c.allRuns / n, c.allIdx / n / 1000.0, c.skinRuns / n, c.skinIdx / n / 1000.0, DlFaceMinIdx(), c.smallRuns / n, c.smallIdx / n / 1000.0, c.otherRuns / n,
                  c.otherIdx / n / 1000.0, c.noCullIdx / n / 1000.0, c.noEyeIdx / n / 1000.0, c.sleepGainRuns / n, c.sleepGainIdx / n / 1000.0, c.sleepInstRuns / n,
                  c.sleepInstIdx / n / 1000.0, c.conflictIdx / n / 1000.0, c.budgetIdx / n / 1000.0, c.soonIdx / n / 1000.0);
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    g_fc = FaceCtr{};
}

// GXV_DLVCACHE: whether the picture of a draw with this render state cannot depend on the order of its own triangles, but for depth ties.
inline bool OrderFreeState(const RenderState& rs) {
    return !rs.blend && rs.depthTest && rs.depthWrite &&
           (rs.depthFunc == CMP_LESS || rs.depthFunc == CMP_LEQUAL || rs.depthFunc == CMP_GREATER || rs.depthFunc == CMP_GEQUAL);
}

#include "vgx_geopass.inc" // (geopass23: GXV_OCCLCULL, GXV_GEOPASS)
#include "vgx_bglayer.inc" // (bglayer25: GXV_BGLAYER=2, the far-background census)

// (streamc31: the stream-decode census's clock, vgx_state.cpp; FeClock's read)
uint64_t DecCenNs() { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(FeClock::Read().time_since_epoch()).count()); }
bool EmitDraw(uint8_t gxPrim, uint32_t layoutMask, uint32_t count, uint32_t foldedFrom, const uint8_t* copyFrom) {
    DynCenScope dynCen(gxPrim, count); // (GXV_DYN_CENSUS)
    const bool fe = workload::FeSplitOn(); // (fixnow31: one read for the draw's scopes)
    // (prewarm24: inside the warm-up scope the draw is resolved as always - the keys, the render state, the folding, a line's widening, the
    // texture bake's choice: everything its variant is made of - but nothing that only serves its record or a cull runs, and it is not drawn)
    const bool warm = g_warmScope;
    if (!warm) OcclDepthNote(); // (GXV_OCCLCULL: a draw that may raise the stored depth ends the occluders' epoch)
#if defined(VGX_CLONE_B)
    two::Abandon(1); // (GXV_FE_TWOCORE: the second worker does no dynamic draw)
#endif
    VtxLayout L = LayoutOf(layoutMask);
    { workload::FeScope feB(fe, workload::FeBook); g_buf.vtx.resize(g_pendingVtxStart + static_cast<size_t>(count) * L.stride); }
    auto drop = [&] { workload::FeScope feB(fe, workload::FeBook); g_buf.vtx.resize(g_pendingVtxStart); if (fepar::Pending()) fepar::Cancel(); };
    if (count == 0) return true;
    // (GXV_SPLIT_COPY, from Producdevity's fork: the vertices are a copy of `copyFrom` the finishing thread makes, split::Copy below; the
    // player makes it itself, here, before anything of its own reads them: place())
    const uint32_t copyBytes = count * L.stride;
    // (GXV_FE_PARALLEL, vgx_fepar.h: the vertices may also be a decode the finishing thread is to make; place() makes it here, as before)
    auto place = [&] {
        if (copyFrom) std::memcpy(g_buf.vtx.data() + g_pendingVtxStart, copyFrom, copyBytes);
        copyFrom = nullptr;
        if (fepar::Pending()) fepar::Place();
    };
    auto later = [&] { return copyFrom != nullptr || fepar::Pending(); }; // (the vertices are not made yet)
    if (!split::g_on) place();

    uint8_t prim = PrimKind(gxPrim);
    uint32_t idxCount = PrimIndexCount(gxPrim, count);
    if (idxCount == 0) { drop(); return true; }

    uint32_t vcdMtxMask = 0;
    if (cur.valid && g.vcdGen == cur.vcdGen) {
        vcdMtxMask = cur.vcdMtxMask; // (the descriptor has not changed since ResolveState read it)
    } else {
        for (int i = 0; i <= 8; ++i)
            if (g.vcd[i]) vcdMtxMask |= 1u << i;
    }
    // (GXV_FIX_LINES: a line or point is widened by the vertex shader, which transforms its vertices: not on the CPU)
    const bool lpWant = prim != PRIM_TRIANGLES && LinesFix();
    { workload::FeScope feR(fe, workload::FeResolve); ResolveState(vcdMtxMask, lpWant ? 0xFFFFFFFFu : count); }
    if (foldedFrom != 0xFFFFFFFFu && !cur.fold) {
        // (laid out for folding, and it does not fold: undo the reservation, the caller decodes again)
        RestoreBvSnapshot();
        g_bvSnap.pendingStart = ~static_cast<size_t>(0);
        return false;
    }
    if (__builtin_expect(GeoPassMode() != 0, 0)) { ++g_geoPh[g_geoPhase].calls; g_geoPh[g_geoPhase].idx += idxCount; } // (GXV_GEOPASS)
    if (__builtin_expect((BgLayerMode() & 2) != 0, 0)) BgCatCount(idxCount); // (GXV_BGLAYER census)
    if (cur.cullAll && prim == PRIM_TRIANGLES) { drop(); return true; }
    { workload::FeScope feC(fe, workload::FeCull); // (GXV_FE_SPLIT: the culls, DynDrawOutside and the occluder test)
    if (const uint32_t dmin = DynCullMin(); dmin && prim == PRIM_TRIANGLES && !lpWant && count >= dmin && !warm) {
        // (GXV_DYNCULL: all vertices outside one plane of the clip volume: no pixel; left out as `cullAll` leaves a draw out)
        if (later()) {
            ++g_cc.dynLater;
        } else {
            ++g_cc.dynTested;
            if (DynDrawOutside(g_buf.vtx.data() + g_pendingVtxStart, count, L.stride)) {
                ++g_cc.dynCulled;
                g_cc.dynIdx += idxCount;
                drop();
                return true;
            }
        }
    }
    if (__builtin_expect(g_occ.n != 0, 0) && prim == PRIM_TRIANGLES && !lpWant && !warm) {
        // (GXV_OCCLCULL: every vertex in one occluder's shadow: every fragment fails the depth test; left out as `cullAll` leaves a draw out)
        if (later()) {
            ++g_oc.dLater;
        } else {
            ++g_oc.dTested;
            if (DynDrawOccluded(g_buf.vtx.data() + g_pendingVtxStart, count, L.stride)) {
                ++g_oc.dCulled;
                g_oc.dIdx += idxCount;
                ++g_geoPh[g_geoPhase].occCalls;
                g_geoPh[g_geoPhase].occIdx += idxCount;
                if (OcclCullMode() == 1) { drop(); return true; }
            }
        }
    }
    }
    // (pix23: GXV_PIX_ZERO, a draw whose values leave every pixel as it was, left out as `cullAll` leaves a draw out; the counts)
    uint8_t pixZero;
    {
        workload::FeScope feC(fe, workload::FeCull);
        if (cur.fold && PixZeroMode() != 0 && !warm) PixFoldRecheck();
        pixZero = cur.pixZero;
        if (cur.pixVtx && !pixZero && count <= 256 && prim == PRIM_TRIANGLES && !later() && PixZeroMode() != 0 && !warm)
            pixZero = PixVtxRecheck(g_buf.vtx.data() + g_pendingVtxStart, count, L);
    }
    if ((PixCensusOn() || PixBlendOffMode() != 0) && cur.soft && prim == PRIM_TRIANGLES && count <= 256 && !later() && !warm) { // (gpu960: the bounds of a small CPU-transformed draw)
        workload::FeScope feSm(fe, workload::FeSmall);
        EfbRect bb;
        bool rc = false;
        if (SmallDrawBounds(gxPrim, g_buf.vtx.data() + g_pendingVtxStart, count, L.stride, bb, rc))
            g_pixFrac = static_cast<float>(bb.w) * static_cast<float>(bb.h) / (static_cast<float>(std::max<int>(g.efbW, 1)) * static_cast<float>(std::max<int>(g.efbH, 1)));
    }
    if (!warm) { workload::FeScope feC(fe, workload::FeCull); if (PixDrop(idxCount, pixZero)) { drop(); return true; } }
    split::DrawJob sfb;// (GXV_SHADOWFILL_BOUNDS: the pixels of a draw that writes alpha, from the model-space vertices)
    if (!lpWant && prim == PRIM_TRIANGLES && !warm) {
        workload::FeScope feN(fe, workload::FeNote);
        if (later() && SfBoundsWanted() && SfAlphaOther()) place(); // (SfDynamicBounds reads them)
        sfb.prim = prim;
        SfDynamicBounds(sfb, g_buf.vtx.data() + g_pendingVtxStart, count, L.stride);
    }
    if (ChaOn() && !warm) {
        workload::FeScope feN(fe, workload::FeNote);
        if (later() && CharaNoteReads()) place();
        CharaNote(later() ? nullptr : g_buf.vtx.data() + g_pendingVtxStart, count, L.stride, L.mask); // (GXV_CHARAA: model-space vertices still)
    }
    RenderState lpRs;
    split::DrawStateRef lpState{};
    bool lpDraw = false;
    if (lpWant) {
        place(); // (GXV_SPLIT_COPY: the lines' vertices are read and widened here)
        LpResult lp;
        const int e = ExpandLinePoints(gxPrim, layoutMask, count, lp);
        if (e == 2) return true; // (the arena refused the vertices; nothing is pending)
        if (e == 1) {
            // (quads: GX does not cull lines, so no face is culled)
            lpRs = cur.rs;
            lpRs.cull = CULL_NONE;
            lpState = split::DrawStateRef{lp.vsId, cur.useFs, cur.texMask, cur.stateSerial, &lpRs, cur.tex};
            lpDraw = true;
            layoutMask = lp.mask;
            count = lp.count;
            L = LayoutOf(layoutMask);
            gxPrim = 0x80;
            prim = PRIM_TRIANGLES;
            idxCount = PrimIndexCount(gxPrim, count);
        }
    }
    // GXV_TEXBAKE: the texture matrices of a CPU-transformed draw into its coordinates (its block holds identity rows for them)
    if (cur.soft && TexBakeOn()) {
        workload::FeScope feF(fe, workload::FeFold);
        static uint16_t bakeVs = 0xFFFF;
        static uint8_t bakeMask = 0;
        if (bakeVs != cur.vsId) { bakeMask = TexBakeMask(cur.vs); bakeVs = cur.vsId; }
        if (bakeMask) place(); // (GXV_SPLIT_COPY: BakeTexCoords rewrites them in place)
        if (bakeMask && !BakeTexCoords(g_buf.vtx.data() + g_pendingVtxStart, count, L, bakeMask)) {
            // (a texgen reads an attribute the vertices do not have, so the coordinate cannot carry the matrix: this primitive is
            // drawn as a large one is, transformed by the vertex shader with the real matrices; not folded either, since folding
            // needs the CPU transform. The next draw's resolution sees soft != cur.soft and builds its blocks again.)
            ++g_texBakeUnsoft;
            VGX_LOG_N(8, "texbake: vertex shader %u reads a texture coordinate the layout %03X lacks: drawn without the CPU transform\n", cur.vsId, layoutMask);
            cur.soft = false;
            cur.mtxRowsUsed = BlkMtx(cur.vs, false);
            if (cur.fold) {
                cur.fold = false;
                cur.useFs = PixUseFs(cur.fsId, cur.rs);
                BlkTev(cur.fs, cur.texW, cur.texH, false);
            }
        }
    }
    // (front-end split: the finishing thread transforms the vertices, once they are where they stay, below)
    const bool split = split::g_on;
    if (cur.soft && !split && !warm) { workload::FeScope feX(fe, workload::FeSoft); SoftTransform(g_buf.vtx.data() + g_pendingVtxStart, count, L); }
    // Where its pixels are, for the bookkeeping of the EFB's contents (pass isolation): worked out
    // for the small draws that are not depth-tested (screen-space quads: post-processing, HUD).
    EfbRect ext{cur.rs.scX, cur.rs.scY, cur.rs.scW, cur.rs.scH};
    uint8_t cover = 0;
    uint8_t zRect = 0;  // (halo31: DrawRec::depthMark bit 1, below)
    if (cur.soft && prim == PRIM_TRIANGLES && count <= 16 && (!cur.rs.depthTest || cur.rs.depthFunc == CMP_ALWAYS) && !warm) {
        workload::FeScope feSm(fe, workload::FeSmall); // (the transformed copy's SoftTransform, timed in soft-xform too, is inside)
        place(); // (GXV_SPLIT_COPY: the bounds are worked out from them here)
        EfbRect box;
        bool rect = false;
        const uint8_t* bv = g_buf.vtx.data() + g_pendingVtxStart;
        alignas(16) uint8_t eye[16 * 112]; // (split: a transformed copy; 16 vertices of at most 108 bytes)
        if (split) {
            std::memcpy(eye, bv, static_cast<size_t>(count) * L.stride);
            { workload::FeScope feX(fe, workload::FeSoft); SoftTransform(eye, count, L); }
            bv = eye;
        }
        int face = 0;
        if (SmallDrawBounds(gxPrim, bv, count, L.stride, box, rect, &face)) {
            ext = box;
            const RenderState& rs = cur.rs;
            const bool whole = rect && !rs.blend && rs.colorWrite && !cur.fs.alphaTestOn && box.w > 0 && box.h > 0;
            cover = (whole && rs.cull == CULL_NONE) ? 1 : 0;
            // (GXV_SHADOW_CULLCOVER, gpuab23: the same rectangle under a cull mode that keeps it - the game's EFB clear quad,
            // MainLoopFramework::clearEfb, is drawn with GX_CULL_BACK and wound clockwise - is noted apart (value 2, DrawRec::coverCull),
            // for the alpha bookkeeping of GXV_SHADOWFILL_BOUNDS / GXV_SHADOW_RECT only; DrawRec::cover, which the back end reads, stays 0)
            // (gpucut23, GXV_FIRSTCLEAR: the back end's depth plan takes such a rectangle as a whole-depth overwrite when it writes depth with
            // ALWAYS; the mark is the same, and its shadow-fill readers act only while GXV_SHADOW_RECT / GXV_SHADOWFILL_BOUNDS are on)
            if (whole && !cover && (ShadowCullCover() || FirstClearMark() || DirectDisplayMark() || ClearQMode() != 0 || ShadowStencilMode() != 0 || SilhGateMode() != 0) && ((rs.cull == CULL_BACK && face > 0) || (rs.cull == CULL_FRONT && face < 0))) cover = 2;
            // (halo31, 18 Nov: DrawRec::depthMark bit 1. MR::clearZBuffer's quad writes depth only, so it is no `cover` (that needs colourWrite) and the back end's
            // GXV_HRW_KEEPDEPTH plan, which asked for `cover`, never found the game's depth clear: console6 and the emulator refused every pair, "last depth-only
            // clear at command -1". The same rectangle rules as `cover`, for a quad that is drawn under its cull mode and writes depth with ALWAYS and neither colour
            // nor alpha. Read only by that plan (PlanFrame's keepdepth flags); a record's merge keys already compare depthMark.)
            if (rect && !rs.blend && !rs.colorWrite && !rs.alphaWrite && !cur.fs.alphaTestOn && box.w > 0 && box.h > 0 && rs.depthTest && rs.depthFunc == CMP_ALWAYS && rs.depthWrite &&
                (rs.cull == CULL_NONE || (rs.cull == CULL_BACK && face > 0) || (rs.cull == CULL_FRONT && face < 0))) zRect = 2;
            if (count == 4 && gxPrim == 0x80 && ClearQMode() != 0) { // (GXV_CLEARQ)
                // a depth-only clear (DrawUtil::clearZBuffer: colour and alpha not written) is no `cover` (it covers no colour): the same rectangle rules
                const bool drawn = rs.cull == CULL_NONE || (rs.cull == CULL_BACK && face > 0) || (rs.cull == CULL_FRONT && face < 0);
                if (cover) cover = static_cast<uint8_t>(cover | ClearQMark(cur.fs, rs, box));
                else if (drawn && rect && !rs.blend && !rs.colorWrite && !rs.alphaWrite && !cur.fs.alphaTestOn && box.w > 0 && box.h > 0) cover = static_cast<uint8_t>(ClearQMark(cur.fs, rs, box) & ~1u); // (bits 3-5 only)
            }
        }
    }
    const uint32_t layoutUnfolded = layoutMask; // (prewarm24: the layout of the same draw drawn without folding)
    if (cur.fold) {
        workload::FeScope feF(fe, workload::FeFold);
        place(); // (GXV_SPLIT_COPY: FoldRegs reads the colours)
        const uint32_t m = FoldRegs(layoutMask, count);
        if (m == 0xFFFFFFFFu) {
            // (cannot happen with the callers there are: draw it unfolded, with the registers in the block)
            cur.fold = false;
            cur.useFs = PixUseFs(cur.fsId, cur.rs);
            BlkTev(cur.fs, cur.texW, cur.texH, false);
        } else if (m != layoutMask) {
            layoutMask = m;
            L = LayoutOf(layoutMask);
        }
    }
    g_bvSnap.pendingStart = ~static_cast<size_t>(0);
    if (__builtin_expect(warm, 0)) {
        // (prewarm24) the variant the record would have, and its palette twin (GXV_RIGIDMERGE: what FinishOrSend notes for a record); then not
        // drawn. A folded draw of a primitive whose vertex count the game chooses (a strip, a fan, triangles: JParticle's stripes, 2 vertices a
        // particle) is also noted unfolded: the same state drawn with more than GXV_SOFTXFORM vertices is not CPU-transformed, so not folded,
        // and is recorded with the key's own fragment shader and the decoded layout. A quad list (0x80: the particles' one-quad lists) is
        // always the same few vertices.
        ++g_pw.draws;
        const uint16_t vs = lpDraw ? lpState.vsId : cur.vsId;
        const RenderState& rs = lpDraw ? lpRs : cur.rs;
        const uint32_t bk = BlendKey(rs);
        WarmNote(vs, cur.useFs, layoutMask, bk);
        const uint16_t twin = (prim == PRIM_TRIANGLES && !lpDraw) ? RigidTwin() : static_cast<uint16_t>(0xFFFF);
        if (twin != 0xFFFF) WarmNote(twin, cur.useFs, layoutMask, bk);
        if (cur.fold && !lpDraw && gxPrim != 0x80) {
            ++g_pw.alt;
            RenderState urs = cur.rs;
            const uint16_t ufs = PixUseFs(cur.fsId, urs);
            WarmNote(vs, ufs, layoutUnfolded, BlendKey(urs));
            if (twin != 0xFFFF) WarmNote(twin, ufs, layoutUnfolded, BlendKey(urs));
        }
        drop();
        return true;
    }
    // (a record holds at most 65,535 vertices: such a primitive cannot be merged either)
    if (count > 0xFFFF) { drop(); return true; }
    if (copyFrom) split::Copy(g_pendingVtxStart, copyFrom, copyBytes); // (GXV_SPLIT_COPY: before the transform and the job that read them)
    if (fepar::Pending()) fepar::Send(); // (GXV_FE_PARALLEL: the decode, likewise)
    if (cur.soft && split) { workload::FeScope feX(fe, workload::FeSoft); split::Xform(g_pendingVtxStart, count, layoutMask); }

    split::DrawJob j;
    { workload::FeScope feB(fe, workload::FeBook); // (the job's fields)
    j.kind = split::JOB_DYNAMIC;
    j.gxPrim = gxPrim;
    j.prim = prim;
    j.cover = cover | HudJobBit();
    j.owner = g_ownerKey; // (GXV_HRW)
    j.playerMark = g_playerMarkDepth > 0 ? 1u : 0u; // (g30)
    j.depthMark = static_cast<uint8_t>((g_depthMarkNext ? 1 : 0) | zRect); // (hrw41; halo31: bit 1 = a depth-only covering rectangle)
    j.clearRgba = (cover & 8u) ? g_cqRgba : 0u; // (GXV_CLEARQ)
    j.layoutMask = layoutMask;
    j.count = count;
    j.idxCount = idxCount;
    const bool abs = AbsoluteIndices();
    j.chunk = abs ? static_cast<uint16_t>(g_buf.vtxChunks.size() - 1) : 0;
    j.first = abs ? g_pendingFirst : 0;
    j.vtxStart = static_cast<uint32_t>(g_pendingVtxStart);
    j.stride = L.stride;
    j.exX = ext.x; j.exY = ext.y; j.exW = ext.w; j.exH = ext.h;
    if (sfb.ab && !lpDraw) { j.ab = 1; j.abX = sfb.abX; j.abY = sfb.abY; j.abW = sfb.abW; j.abH = sfb.abH; }
    if (prim == PRIM_TRIANGLES && !lpDraw) {
        // (a CPU-transformed draw too: its block is the identity, which is a palette slot like any other matrix)
        j.vsTwin = RigidTwin(); // (GXV_RIGIDMERGE; 0xFFFF: off or not a rigid key)
        const uintptr_t vp = reinterpret_cast<uintptr_t>(g_buf.vtx.data() + g_pendingVtxStart);
        j.vtxPtrLo = static_cast<uint32_t>(vp);
        j.vtxPtrHi = static_cast<uint32_t>(static_cast<uint64_t>(vp) >> 32);
    }
    }
    { workload::FeScope feS(fe, workload::FeSend); FinishOrSend(j, lpDraw ? &lpState : nullptr); }
    workload::FeScope feB2(fe, workload::FeBook);
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

// ================================================================ static runs outside the view (GXV_STATICCULL)
// The map's models (the Comet Observatory's base, the planets, the domes) are each one joint and a shape per material
// (AstroBaseA: 20 shapes, 20 materials, one joint; AstroBaseCenterA: 5, 5, 1): every shape is a cached display list drawn
// from its static buffer, about 265 of them a picture, and each one costs the player its resolution (keys, render state,
// textures, the blocks), the job for the finishing thread, a record and a GL draw, whether or not the shape is in view.
// The game culls whole actors by a bounding sphere only, and a base or a planet is always inside it. Here a static run
// whose vertices all lie outside one plane of the clip volume (left, right, bottom, top, or behind the eye) is not drawn
// at all: no resolution, no job, no record. A triangle whose three vertices are outside the same plane of the clip volume
// is clipped away whole, so the picture is the same, bit for bit (GL clips every primitive to the volume).
//   * The bounds are the list's own: the model-space box of its decoded vertices (RowBoxesOf, worked out once per list when
//     it is built, from the vertices its static buffer holds), through the position rows of the current matrix index and
//     the projection registers (XF 0x20..0x26): the very numbers BuildMtxC / BuildProjC put into u_mv and u_proj, which the
//     vertex shader multiplies the same vertices by (gl_Position = u_proj * (u_mv * (x, y, z, 1))). The transform is
//     affine, so every vertex is inside the convex hull of the box's eight corners, in clip space as well.
//   * Only triangles, only without per-vertex matrices (no PNMTXIDX: one matrix for every vertex), only static runs (never
//     CPU-transformed). A corner counts as outside a plane only by a margin of 1e-4 of the magnitudes summed to compute it
//     (some 800 single-precision rounding steps): the GPU's own rounding moves a vertex by far less. A non-finite number
//     anywhere fails every test (nothing is culled).
//   * The four side planes are moved out by 8 EFB pixels and used only where the scissor lies within 4 pixels of the viewport
//     (the game's 3D passes): so the result holds on a GPU that clips to the scissor only, not to the viewport (GXM's region
//     clip, vitaGL), and with vitaGL's viewport one pixel narrower at odd sizes. "Behind the eye" needs neither (the near plane).
//   * The first use of a list (after it is built) is always drawn, so that the programs a stage needs are linked when it is
//     entered, as before, and not when a shape first comes into view.
//   * The state commands before the draw are applied as before; only the draw's resolution is left out, and ResolveState
//     is lazy (dirty flags and memos): the next draw resolves what this one would have left, with the same result. What
//     the resolution would have consumed (the pending palette class, the pending bounds) is consumed here.
//   GXV_STATICCULL=1 (default) on; 0 off, exactly as before (the lists' bounds are then not even computed, unless
//   GXV_CHARAA). Read once. The counters: StaticCullCounters, and (ta23) the "ta23:" basic-log line every 10 s (CullLog), which
//   also counts GXV_STATICCULL_PAL (runs with per-vertex matrices) and GXV_DYNCULL (dynamic draws). The code is above EmitDraw.

#define VGX_DJ_PART2
#include "vgx_direct_draw.inc" // (GXV_DIRECT_J3D: dj::SyncForNotes)
#undef VGX_DJ_PART2
void EmitStaticDraw(uint8_t gxPrim, uint32_t layoutMask, uint32_t count, uint32_t bufId, uint32_t vtxByteOffset, uint32_t idxOffset,
                    uint32_t vtxFirst) {
#if defined(VGX_CLONE_B)
    two::Abandon(1);
#endif
    const bool fe = workload::FeSplitOn(); // (fixnow31: one read for the draw's scopes)
    workload::FeScope feStatic(fe, workload::FeStatic);
    const bool warm = g_warmScope; // (prewarm24: resolved, its variant noted, not drawn; see EmitDraw)
    if (!warm) OcclDepthNote(); // (GXV_OCCLCULL)
    const uint8_t prim = PrimKind(gxPrim);
    const uint32_t idxCount = PrimIndexCount(gxPrim, count);
    if (count == 0 || idxCount == 0 || count > 0xFFFF) return;
    const uint32_t vcdMtxMask = VcdMtxMask();
    // Larger than the CPU-transform limit by construction (see vgx_dlcache.cpp), so never "soft".
    { workload::FeScope feR(fe, workload::FeResolve); ResolveState(vcdMtxMask, 0xFFFFFFFFu); }
    if (cur.cullAll && prim == PRIM_TRIANGLES) return;
    if (__builtin_expect(warm, 0)) {
        ++g_pw.draws;
        WarmNote(cur.vsId, cur.useFs, layoutMask, BlendKey(cur.rs));
        if (g_pendingCopyable && prim == PRIM_TRIANGLES) { const uint16_t twin = RigidTwin(); if (twin != 0xFFFF) WarmNote(twin, cur.useFs, layoutMask, BlendKey(cur.rs)); }
        return;
    }
    PixCoverStatic(); // (gpu960)
    if (PixDrop(idxCount)) return; // (pix23: GXV_PIX_ZERO)
    split::DrawJob j;
    j.prim = prim;
    SfStaticBounds(j, g_cha.pend, g_cha.pendN); // (GXV_SHADOWFILL_BOUNDS: before CharaNote takes the list's bounds)
    if (ChaOn()) CharaNote(nullptr, 0, 0, 0); // (GXV_CHARAA: the list's bounds, SetPendingBounds)
    j.kind = split::JOB_STATIC;
    j.cover = HudJobBit();
    j.owner = g_ownerKey; // (GXV_HRW)
    j.playerMark = g_playerMarkDepth > 0 ? 1u : 0u; // (g30)
    j.depthMark = g_depthMarkNext ? 1 : 0; // (hrw41)
    j.gxPrim = gxPrim;
    j.prim = prim;
    j.layoutMask = layoutMask;
    j.count = count;
    j.idxCount = idxCount;
    j.vbuf = bufId;
    j.vtxOffset = vtxByteOffset;
    j.vtxFirst = vtxFirst;
    j.idxOffset = idxOffset;
    if (g_pendingCopyable && prim == PRIM_TRIANGLES) { j.copyable = 1; j.vsTwin = RigidTwin(); } // (GXV_RIGIDMERGE_DLLEARN)
    { workload::FeScope feS(fe, workload::FeSend); FinishOrSend(j); }
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

// GXV_FE_CENSUS=1 (fe23; read once, default 0): the static runs that repeat the run just before them (the same static buffer and range,
// nothing sent between them, the same shaders, render state serial and textures: the same model's shape for another instance, as
// DrawBufferShapeDrawer sends the flower groups' plants), counted with the length of such repeats; vgx_dlcache.cpp's census prints them.
namespace {
struct StaticRepeat {
    uint64_t runs = 0, repeats = 0, rigid = 0, len23 = 0, len47 = 0, len8 = 0, inRuns = 0;
    uint32_t bufId = 0, idxOffset = 0, vtxFirst = 0, layoutMask = 0, serial = 0, jobSeq = 0, len = 0;
    uint16_t vsId = 0, useFs = 0;
    bool valid = false;
} g_sr;
inline bool FeCensusOn() { return VGX_KNOB(EnvInt("GXV_FE_CENSUS", 0) != 0) != 0; }
void StaticRepeatClose() {
    if (g_sr.len >= 2) {
        if (g_sr.len <= 3) ++g_sr.len23;
        else if (g_sr.len <= 7) ++g_sr.len47;
        else ++g_sr.len8;
        g_sr.inRuns += g_sr.len;
    }
    g_sr.len = 0;
}
__attribute__((noinline)) void StaticRepeatNote(uint32_t layoutMask, uint32_t bufId, uint32_t idxOffset, uint32_t vtxFirst) {
    dj::SyncKeys(); // (GXV_DIRECT_J3D: cur.vs is read below)
    StaticRepeat& r = g_sr;
    ++r.runs;
    const bool rep = r.valid && r.jobSeq == g_jobSeq && r.bufId == bufId && r.idxOffset == idxOffset && r.vtxFirst == vtxFirst && r.layoutMask == layoutMask &&
                     r.serial == cur.stateSerial && r.vsId == cur.vsId && r.useFs == cur.useFs;
    if (rep) {
        ++r.repeats;
        if (cur.vs.perVtxPos == 0) ++r.rigid;
        r.len = r.len ? r.len + 1 : 2;
    } else {
        StaticRepeatClose();
    }
    r.valid = true;
    r.bufId = bufId; r.idxOffset = idxOffset; r.vtxFirst = vtxFirst; r.layoutMask = layoutMask;
    r.serial = cur.stateSerial; r.vsId = cur.vsId; r.useFs = cur.useFs;
    r.jobSeq = g_jobSeq + 1; // (FinishOrSend counts this run's job next: a repeat must follow it with nothing sent between)
}
} // namespace
void StaticRepeatCounters(uint64_t out[8]) {
    out[0] = g_sr.runs; out[1] = g_sr.repeats; out[2] = g_sr.rigid; out[3] = g_sr.len23; out[4] = g_sr.len47; out[5] = g_sr.len8; out[6] = g_sr.inRuns; out[7] = 0;
}

// ---- GXV_PALJOIN_CENSUS=1 (paljoin31, plan-gpu31 A-T1; read once, default 0; log only): how many of a picture's skinned static draws could be one draw
// if the matrix groups of a shape shared one palette per model. The decision is vgx_paljoin.h (read its head for the rules and the exactness argument);
// this is the part that reads the renderer: the rows a run names (the list's per-row boxes, else slots 0..highest row), the matrices' contents,
// the register file's dirty flags, the model's array bases, and the run's own front-end time (EmitStaticRun's whole body, less the census's own work).
// A "pal-census:" line every GXV_PALJOIN_CENSUS_MS (5000) ms. Needs the static runs to be made by this file: the native J3D chains (GXV_J3DNATIVE=4/5)
// and the second front-end worker refuse while it is on, as they do for GXV_FE_CENSUS.
namespace {
paljoin::Census g_pj;
inline bool PalJoinOn() { static const bool on = VGX_KNOB(EnvInt("GXV_PALJOIN_CENSUS", 0) != 0) != 0; return on; }
struct PjScope {
    bool on, noted = false;
    paljoin::Verdict v;
    FeClock::Time t0{};
    int64_t ownNs = 0;
    const RowBox* pend = nullptr;
    uint32_t pendN = 0, maxRow = 0xFF;
    bool regs = false;
    PjScope() : on(PalJoinOn()) {
        if (!on) return;
        t0 = FeClock::Read();
        pend = g_cha.pend;
        pendN = g_cha.pendN;
        maxRow = g_pendingPosMtxMax;
        // (a register other than matrix memory written since the last run that was drawn: ResolveState clears these)
        regs = g.bpDirty || g.bpKeyDirty || g.bpRsDirty || g.xfRegDirty || g.xfKeyDirty || g.xfViewDirty || g.xfProjDirty || g.xfColorDirty || g.xfLightDirty ||
               g.texDirty || g.tevColDirty != 0 || g.texSlotDirty != 0;
        g_pj.StaticRun();
    }
    ~PjScope() {
        if (!on || !noted) return;
        const int64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(FeClock::Read() - t0).count() - ownNs;
        g_pj.AddNs(v, ns > 0 ? static_cast<uint64_t>(ns) : 0);
        g_pj.Sent(g_jobSeq);
    }
};
// What the run names: its matrices' contents (a position row triple and, for a shape that reads normals, its normal rows), in slot order.
__attribute__((noinline)) void PalJoinNote(PjScope& s, uint32_t layoutMask, uint32_t vtxCount, uint32_t idxCount) {
    const FeClock::Time a = FeClock::Read();
    dj::SyncKeys(); // (GXV_DIRECT_J3D: cur.vs is read below)
    if (cur.vs.perVtxPos != 0) {
        paljoin::Draw d;
        d.vertices = vtxCount;
        d.indices = idxCount;
        d.palClass = cur.vs.perVtxPos;
        d.serial = cur.stateSerial;
        d.useFs = cur.useFs;
        d.jobSeq = g_jobSeq;
        d.regsWritten = s.regs;
        d.model = (static_cast<uint64_t>(g.cpBase[0]) << 32) | g.cpBase[1];
        VsKey vk = cur.vs;
        vk.perVtxPos = 1; // (the palette class is chosen again from the joined palette)
        d.vsNorm = paljoin::HashBytes(&layoutMask, sizeof layoutMask, paljoin::HashBytes(&vk, sizeof vk));
        bool lit = false, texPal = false;
        for (int i = 0; i < 4; ++i) lit |= (cur.vs.chan[i].enable != 0 && cur.vs.chan[i].lightMask != 0);
        for (int i = 0; i < cur.vs.numTexGens; ++i) {
            lit |= (cur.vs.tg[i].type < 2 && cur.vs.tg[i].src == 1);
            texPal |= cur.vs.tg[i].perVtxMtx != 0;
        }
        d.lit = lit;
        uint32_t rows[paljoin::kMaxSlots + 2];
        uint32_t nRows = 0;
        if (texPal) d.ineligible = paljoin::InTexgenPal;
        else if (s.pend && s.pendN > 1) { // (boxes 1..n: one per row the list's vertices name)
            for (uint32_t k = 1; k < s.pendN && d.ineligible == paljoin::InOk; ++k) {
                const uint32_t r = s.pend[k].row;
                if (r > 61) d.ineligible = paljoin::InRowAbove61;
                else if (nRows < paljoin::kMaxSlots) rows[nRows++] = r;
            }
        } else if (s.maxRow != 0xFF) { // (no per-row boxes: every slot up to the highest row: an upper bound)
            if (s.maxRow > 61) d.ineligible = paljoin::InRowAbove61;
            else
                for (uint32_t r = 0; r <= s.maxRow && nRows < paljoin::kMaxSlots; r += 3) rows[nRows++] = r;
        } else {
            d.ineligible = paljoin::InRowsUnknown;
        }
        if (d.ineligible == paljoin::InOk) {
            std::sort(rows, rows + nRows);
            nRows = static_cast<uint32_t>(std::unique(rows, rows + nRows) - rows);
            uint64_t image = 1469598103934665603ull;
            for (uint32_t i = 0; i < nRows; ++i) {
                const uint32_t r = rows[i];
                uint32_t w[21] = {};
                for (uint32_t q = 0; q < 3; ++q) {
                    std::memcpy(&w[q * 4], g.xfPos[r + q < 64 ? r + q : 63], 16);
                    if (lit && r + q < 32) std::memcpy(&w[12 + q * 3], g.xfNrm[r + q], 12);
                }
                const uint64_t h = paljoin::HashWords(w, 21);
                image = paljoin::HashWords(reinterpret_cast<const uint32_t*>(&r), 1, image);
                image = paljoin::HashWords(reinterpret_cast<const uint32_t*>(&h), 2, image);
                bool have = false;
                for (uint8_t j = 0; j < d.nMat && !have; ++j) have = d.mat[j] == h;
                if (!have) d.mat[d.nMat++] = h;
            }
            d.image = image;
        }
        s.v = g_pj.Note(d);
        s.noted = true;
    }
    s.ownNs += std::chrono::duration_cast<std::chrono::nanoseconds>(FeClock::Read() - a).count();
}
// The "pal-census:" lines (called once a picture, FrontPresent).
void PalJoinLog() {
    if (!PalJoinOn()) return;
    g_pj.PictureEnd();
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::milliseconds(EnvInt("GXV_PALJOIN_CENSUS_MS", 5000))) return;
    last = now;
    const paljoin::Counters& c = g_pj.c;
    const double p = static_cast<double>(c.pictures ? c.pictures : 1);
    auto ms = [&](uint64_t ns) { return static_cast<double>(ns) / 1e6 / p; };
    auto per = [&](uint64_t n) { return static_cast<double>(n) / p; };
    uint32_t keysPal = 0, keysAll = static_cast<uint32_t>(g_vsReg.keys.size());
    for (const VsKey& k : g_vsReg.keys) {
        bool pal = k.perVtxPos != 0;
        for (int i = 0; i < k.numTexGens; ++i) pal |= k.tg[i].perVtxMtx != 0;
        keysPal += pal ? 1u : 0u;
    }
    const double relMs = ms(c.removedReloadNs[0]);
    const bool gateN = per(c.removedReload[0]) >= 60.0, gateT = relMs >= 1.5;
    char line[1500];
    int o = std::snprintf(line, sizeof line,
        "pal-census: %llu pictures; per picture: static runs %.1f, palette runs %.1f (full %.1f, short %.1f, small %.1f; upload at most %.1f KB), %.2f matrices a draw (most %llu), "
        "%.0f us front end (%.2f ms) | joinable draws, palette as the shader holds it (10 matrices lit, 21 unlit): %.1f (%.1f after a matrix reload, %.1f with the palette unchanged), %.0f us "
        "(%.2f ms; after a reload %.2f ms), %.0f vertices; up to 21 matrices: %.1f; no size limit: %.1f (%.2f ms) | gate (reload ones >= 60 and >= 1.5 ms): %.1f %s, %.2f ms %s",
        static_cast<unsigned long long>(c.pictures), per(c.staticRuns), per(c.palDraws), per(c.byClass[1]), per(c.byClass[2]), per(c.byClass[3]), per(c.palUploadBytes) / 1024.0,
        c.palDraws ? static_cast<double>(c.mtxSum) / static_cast<double>(c.palDraws) : 0.0, static_cast<unsigned long long>(c.mtxMax), per(c.palNs) / 1e3, ms(c.palNs),
        per(c.removed[0]), per(c.removedReload[0]), per(c.removedSame[0]), per(c.removedNs[0]) / 1e3, ms(c.removedNs[0]), relMs, per(c.removedVerts), per(c.removed[1]),
        per(c.removed[2]), ms(c.removedNs[2]), per(c.removedReload[0]), gateN ? "yes" : "no", relMs, gateT ? "yes" : "no");
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    o = std::snprintf(line, sizeof line, "pal-census: per picture, a palette draw that did not join the one before it (palette as held): ");
    for (int b = 1; b < paljoin::BkCount; ++b)
        if (c.brk[b]) o += std::snprintf(line + o, sizeof line - o, "%s %.1f; ", paljoin::BreakName(b), per(c.brk[b]));
    o += std::snprintf(line + o, sizeof line - o, "| not joinable at all:");
    for (int b = 1; b < paljoin::InCount; ++b)
        if (c.ineligible[b]) o += std::snprintf(line + o, sizeof line - o, " %s %.1f;", paljoin::InelName(b), per(c.ineligible[b]));
    o += std::snprintf(line + o, sizeof line - o, " | chains by draws (1, 2, 3-4, 5-8, 9+), held to 10 matrices: %.1f %.1f %.1f %.1f %.1f; no size limit: %.1f %.1f %.1f %.1f %.1f", per(c.chainLen[0][0]),
                       per(c.chainLen[0][1]), per(c.chainLen[0][2]), per(c.chainLen[0][3]), per(c.chainLen[0][4]), per(c.chainLen[2][0]), per(c.chainLen[2][1]), per(c.chainLen[2][2]),
                       per(c.chainLen[2][3]), per(c.chainLen[2][4]));
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    // (the six models with most joinable draws: position array base, draws, joinable, front end us a picture of all / of the joinable)
    const paljoin::ModelRow* top[6] = {};
    for (int i = 0; i < g_pj.nModels; ++i) {
        const paljoin::ModelRow& m = g_pj.models[i];
        for (int t = 0; t < 6; ++t)
            if (!top[t] || m.removed > top[t]->removed) {
                for (int k = 5; k > t; --k) top[k] = top[k - 1];
                top[t] = &m;
                break;
            }
    }
    o = std::snprintf(line, sizeof line, "pal-census: %d models with palette draws%s; most joinable (array base: draws / joinable / us / joinable us a picture):", g_pj.nModels,
                      g_pj.modelsLost ? " (table full: more not listed)" : "");
    for (int t = 0; t < 6 && top[t]; ++t)
        o += std::snprintf(line + o, sizeof line - o, " [%08llx %.1f/%.1f/%.0f/%.0f]", static_cast<unsigned long long>(top[t]->model >> 32), per(top[t]->draws), per(top[t]->removed),
                           per(top[t]->ns) / 1e3, per(top[t]->removedNs) / 1e3);
    o += std::snprintf(line + o, sizeof line - o, " | vertex keys with a palette: %u of %u (every one needs a twin if the palette becomes a model's)", keysPal, keysAll);
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    g_pj.Reset();
}
} // namespace

// ---- GXV_J3DNATIVE=4 / 5 (vgx_j3native2_draw.inc): what needs the switches and ResolveState defined above
namespace j3n2 {
bool DrawUsable(const char** why) {
    *why = nullptr;
    if (RsCacheMode() != 1) *why = "GXV_RSCACHE is not 1";
    else if (TraceFrame()) *why = "a traced picture";
    else if (stats::Detailed()) *why = "statistics detail";
    else if (PaletteRowsMode() == 2) *why = "the palette census (GXV_PALROWS=2)";
    else if (PixAny()) *why = "a gpu960 pixel switch (GXV_PIX_*)";
    else if (DiagFetchOn()) *why = "GXV_DIAG_FETCHONLY";
    else if (OcclCullMode() != 0 || GeoPassMode() != 0) *why = "GXV_OCCLCULL / GXV_GEOPASS";
    else if (ShadowFillOn()) *why = "GXV_SHADOWFILL_BOUNDS";
    else if (RigidMergeLearnMax() != 0) *why = "GXV_RIGIDMERGE_DLLEARN";
    else if (FeCensusOn()) *why = "GXV_FE_CENSUS";
    else if (PalJoinOn()) *why = "GXV_PALJOIN_CENSUS";
    else if (VsTrimTracked()) *why = "GXV_VS_TRIM";
    return *why == nullptr;
}
} // namespace j3n2
// EmitStaticRun's resolution of a run while a chain's shape call is in progress (j3n2::g_draw set).
void J3n2ResolveRun(uint32_t vcdMtxMask) {
    using namespace j3n2;
    const TplKey k = KeyNow(vcdMtxMask);
    if (g_draw->mode == kTpl) {
        int why = 0;
        if (TplApply(k, vcdMtxMask, why)) {
            ++g_dctr.templated;
            return;
        }
        ++g_dctr.bails[why];
        Bail(why); // (the register file made real; the chain goes on from here)
    }
    if (__builtin_expect(dj::g_armed != nullptr, 0)) dj::Resolve(vcdMtxMask);
    else ResolveState(vcdMtxMask, 0xFFFFFFFFu);
    Capture(k);
}
void EmitStaticRun(uint8_t prim, uint32_t layoutMask, uint32_t vtxCount, uint32_t idxCount, uint32_t prims, uint32_t bufId, uint32_t vtxByteOffset,
                   uint32_t idxOffset, uint32_t vtxFirst) {
    workload::FeScope feStatic(workload::FeStatic);
    PjScope pj; // (GXV_PALJOIN_CENSUS: before anything consumes the pending bounds, the highest row or the dirty flags)
    const uint32_t idxAlt = g_pendingIdxAlt, idxAltCount = g_pendingIdxAltCount; // (GXV_DLVCACHE: consumed by this call, drawn or not)
    g_pendingIdxAlt = ~0u;
    const uint32_t vqK = g_pendingVq; // (GXV_VTXPACK: likewise)
    g_pendingVq = 0;
    facecull::Run* const face = g_pendingFace; // (GXV_FACECULL: likewise)
    g_pendingFace = nullptr;
    const RowBox* const clusters = g_pendingCl; // (GXV_CLUSTERCULL: likewise)
    const uint32_t clusterN = g_pendingClN;
    g_pendingCl = nullptr;
    g_pendingClN = 0;
    const ta27::RunSet* const t27 = g_pendingT27; // (GXV_TA27: likewise)
    g_pendingT27 = nullptr;
    const OccShape* const occ = g_occPending; // (GXV_OCCLCULL: the run's analysis, consumed by this call)
    g_occPending = nullptr;
    OcclDepthNote(); // (GXV_OCCLCULL: a draw that may raise the stored depth ends the occluders' epoch)
    if (vtxCount == 0 || idxCount == 0 || vtxCount > 0xFFFF) return;
    if (__builtin_expect(GeoPassMode() != 0, 0)) { // (GXV_GEOPASS)
        ++g_geoPh[g_geoPhase].calls;
        g_geoPh[g_geoPhase].idx += idxCount;
        GeoStaticRun(bufId, idxOffset, idxCount, occ);
    }
    if (__builtin_expect((BgLayerMode() & 2) != 0, 0)) BgCatCount(idxCount); // (GXV_BGLAYER census)
    const bool firstUse = g_cha.pend && g_cha.pendN && !g_cha.pend[0].drawn; // (before GXV_STATICCULL marks it)
    if (StaticRunCulled(prim, vtxCount, idxCount)) return; // (GXV_STATICCULL: outside the clip volume, not drawn)
    if (__builtin_expect(g_occ.n != 0, 0) && StaticRunOccluded(prim, idxCount, firstUse)) return; // (GXV_OCCLCULL: hidden behind an occluder)
    const uint32_t vcdMtxMask = VcdMtxMask();
    {
        workload::FeScope feR(workload::FeResolve);
        ++dj::g_resolves; // (GXV_DIRECT_J3D: a static run resolved, either way)
        if (__builtin_expect(j3n2::g_draw != nullptr, 0)) J3n2ResolveRun(vcdMtxMask); // (GXV_J3DNATIVE=4 / 5: a run of a chain's shape call)
        else if (__builtin_expect(dj::g_armed != nullptr, 0)) dj::Resolve(vcdMtxMask); // (GXV_DIRECT_J3D: a J3D shape's run, templated keys)
        else ResolveState(vcdMtxMask, 0xFFFFFFFFu); // (static geometry is transformed by the vertex shader)
    }
    if (cur.cullAll && prim == PRIM_TRIANGLES) return;
    PixCoverStatic(); // (gpu960)
    if (PixDrop(idxCount)) return; // (pix23: GXV_PIX_ZERO)
    split::DrawJob j;
    j.prim = prim;
    if (dj::g_armed) dj::SyncForNotes(); // (the notes below may read the keys)
    // (GXV_SHADOW_RECT_FE: SfStaticBounds returns at once for a draw that cannot write alpha other than 0; whether it can is known from the
    // render state and the fragment key's destination-alpha flag, which dj::CurFs reads without making the deferred copy)
    if (ShadowRectFeMode() == 0 || dj::SfKeysNeeded()) SfStaticBounds(j, g_cha.pend, g_cha.pendN); // (GXV_SHADOWFILL_BOUNDS: before CharaNote takes the list's bounds)
    if (ChaOn()) CharaNote(nullptr, 0, 0, 0); // (GXV_CHARAA: the list's bounds, SetPendingBounds)
    const uint32_t idxOffset0 = idxOffset;
    if (__builtin_expect(Ta27Mode() != 0, 0) && prim == PRIM_TRIANGLES) Ta27Use(t27, layoutMask, vtxCount, idxCount, firstUse); // (GXV_TA27: the census, before the vertex-cache block replaces the indices)
    // (GXV_FACECULL: the run's view list, a static buffer of its own: the triangles that can face the camera and the vertices they use.
    // Not in the warm-up scope, not for a run that may be copied or instanced; a list replaces the vertex-cache block and the cluster ranges.)
    bool faceTook = false;
    const bool faceOffer = face != nullptr && prim == PRIM_TRIANGLES && !g_warmScope && !g_pendingCopyable && g_pendingInst == 0;
    if (__builtin_expect(FaceCullMode() != 0, 0) && prim == PRIM_TRIANGLES && !g_warmScope) { // (the census's second line)
        ++g_fc.allRuns;
        g_fc.allIdx += idxCount;
        if (!faceOffer) {
            if (g.vcd[0] != 0) { ++g_fc.skinRuns; g_fc.skinIdx += idxCount; }
            else if (idxCount < DlFaceMinIdx()) { ++g_fc.smallRuns; g_fc.smallIdx += idxCount; }
            else { ++g_fc.otherRuns; g_fc.otherIdx += idxCount; }
        }
    }
    if (__builtin_expect(faceOffer, 0)) {
        uint32_t fb = 0, fi = 0, fv = 0;
        const FaceResult fr = FaceTake(*face, idxCount, vtxCount, &fb, &fi, &fv);
        if (fr == kFaceNothing) return; // (no triangle of the run can face the camera: left out as `cullAll` leaves a draw out, after ResolveState)
        if (fr == kFaceList) {
            faceTook = true;
            bufId = fb;
            vtxByteOffset = 0;
            vtxFirst = 0;
            idxOffset = 0;
            idxCount = fi;
            vtxCount = fv;
        }
    }
    if (idxAlt != ~0u && prim == PRIM_TRIANGLES && !faceTook) {
        // (GXV_DLVCACHE: the run's triangles in the vertex-cache order, where the order cannot show; see vgx_dlcache.cpp VcacheSize)
        ++g_cc.vcHad;
        g_cc.vcIdxHad += idxCount;
        if (OrderFreeState(cur.rs) && idxAltCount && idxAltCount <= idxCount) {
            ++g_cc.vcTook;
            g_cc.vcIdxTook += idxAltCount;
            idxOffset = idxAlt;
            idxCount = idxAltCount;
        }
    }
    // (GXV_CLUSTERCULL: the run as the ranges of its indices that have a cluster in view; see "clusters of a static run")
    ClRange ranges[4] = {{0, idxCount}};
    uint32_t nRanges = 1;
    if (__builtin_expect(ClusterMode() != 0, 0) && prim == PRIM_TRIANGLES) {
        ClCtr& cc = g_cl;
        ++cc.runs;
        cc.idx += idxCount;
        const bool taken = faceTook || idxOffset != idxOffset0; // (GXV_DLVCACHE's reordered block, or GXV_FACECULL's list, replaced the run's indices)
        if (!clusters || !clusterN || g.vcd[0] != 0) { ++cc.noCl; cc.noClIdx += idxCount; }
        else if (firstUse) { ++cc.firstUse; ++cc.withCl; cc.withClIdx += idxCount; }
        else {
            ++cc.withCl;
            cc.withClIdx += idxCount;
            const bool order = !taken && !g_warmScope && !(occ && OcclCullMode()) && !g_pendingCopyable && g_pendingInst == 0;
            if (order) {
                uint32_t took = 0, pot = 0;
                ClRange rg[4];
                const uint32_t n = ClusterRanges(clusters, clusterN, idxCount, rg, &took, &pot);
                cc.potIdx += pot;
                if (took) { cc.tookIdx += took; ++cc.tookRuns; }
                if (n == 0) { ++cc.none; cc.noneIdx += idxCount; }
                else if (n > 1) cc.extra += n - 1;
                if (ClusterMode() == 3) { // (the ranges tile the run: in order, whole triangles, inside it, the kept plus the left out is the run)
                    uint32_t kept = 0, prevEnd = 0;
                    bool bad = n > 4;
                    for (uint32_t k = 0; k < n && k < 4; ++k) {
                        bad = bad || rg[k].s % 3 != 0 || rg[k].n % 3 != 0 || rg[k].n == 0 || rg[k].s < prevEnd || rg[k].s + rg[k].n > idxCount;
                        prevEnd = rg[k].s + rg[k].n;
                        kept += rg[k].n;
                    }
                    bad = bad || kept + took != idxCount;
                    if (bad) ++cc.rangeBad;
                }
                if (ClusterMode() != 2) {
                    if (n == 0) return; // (no cluster in view: left out as `cullAll` leaves a draw out, after ResolveState)
                    for (uint32_t k = 0; k < n; ++k) ranges[k] = rg[k];
                    nRanges = n;
                }
            }
        }
    }
    uint32_t idxKept = 0;
    for (uint32_t k = 0; k < nRanges; ++k) idxKept += ranges[k].n;
    j.kind = split::JOB_STATIC;
    j.cover = HudJobBit();
    j.owner = g_ownerKey; // (GXV_HRW)
    j.playerMark = g_playerMarkDepth > 0 ? 1u : 0u; // (g30)
    j.depthMark = g_depthMarkNext ? 1 : 0; // (hrw41)
    j.prim = prim;
    j.layoutMask = layoutMask;
    j.count = vtxCount;
    j.idxCount = idxCount;
    j.vbuf = bufId;
    j.vtxOffset = vtxByteOffset;
    j.vtxFirst = vtxFirst;
    j.idxOffset = idxOffset;
    if (vqK) {
        // (GXV_VTXPACK: what the record stage scales the matrix blocks by, and which columns: from the key the draw is made with. With
        // GXV_DIRECT_J3D the copy of the key in cur.vs may still be an earlier shape's (dj: the copies are deferred until something reads
        // them; SyncForNotes above makes them only for the shadow-fill and character notes): the console's 2.3 c12 run took the palette
        // class and texgens of a stale key there, scaled the wrong blocks and drew the map's unscaled whole numbers. SyncKeys makes the copy.)
        dj::SyncKeys();
        j.vqK = vqK;
        j.vqTex = VqTexOf(cur.vs);
        j.vqPal = cur.vs.perVtxPos;
        bool anyPost = false;
        for (int i = 0; i < cur.vs.numTexGens; ++i) anyPost |= cur.vs.tg[i].type < 2 && cur.vs.tg[i].post != 0;
        const uint32_t n = cur.vs.numTexGens;
        j.vqTmLen = static_cast<uint8_t>(!UniformPacking() ? 192u : (anyPost ? 96u + n * 12u : n * 12u)); // (BuildMtxC's block length)
        if (j.vqTex == kVqUnsafe) VqBump(g_vqUnsafe);
    }
    if (g_pendingCopyable && prim == PRIM_TRIANGLES) { j.copyable = 1; j.vsTwin = RigidTwin(); } // (GXV_RIGIDMERGE_DLLEARN)
    else if (g_pendingInst != 0 && prim == PRIM_TRIANGLES) {
        // (GXV_STATIC_INST: the list has an instance buffer; the record stage joins the job to the record before it where that is the same run)
        const uint16_t tw = StaticInstTwin(cur.vsId);
        if (tw != 0xFFFF) { j.vsTwin = tw; j.chunk = g_pendingInst; }
    }
    if (FeCensusOn()) StaticRepeatNote(layoutMask, bufId, idxOffset, vtxFirst); // (GXV_FE_CENSUS)
    if (pj.on) PalJoinNote(pj, layoutMask, vtxCount, idxCount); // (GXV_PALJOIN_CENSUS)
    {
        workload::FeScope feS(workload::FeSend);
        if (__builtin_expect(nRanges == 1 && ranges[0].n == idxCount, 1)) FinishOrSend(j);
        else
            for (uint32_t k = 0; k < nRanges; ++k) { // (GXV_CLUSTERCULL: one job a range of the run's indices, in the run's order)
                split::DrawJob jk = j;
                jk.idxOffset = idxOffset + ranges[k].s;
                jk.idxCount = ranges[k].n;
                FinishOrSend(jk);
            }
    }
    if (occ && occ->orient && prim == PRIM_TRIANGLES && OcclCullMode()) OcclRegister(*occ); // (GXV_OCCLCULL: drawn: an occluder of the draws after it)
    g_frameDraws += prims;
    {
        stats::Frame& fr = stats::Cur();
        fr.prims += prims;
        fr.verts += vtxCount;
        ++fr.staticDraws;
        fr.staticVerts += vtxCount;
        if (prim == PRIM_TRIANGLES) fr.tris += idxKept / 3;
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
        if (c.playerN) mix(c.player, sizeof(EfbRect) * c.playerN); // (mario24: likewise)
        if (c.edge) { const uint32_t e = 0xED6Eu + c.hudN; mix(&e, 4); if (c.hudN) mix(c.hud, sizeof(EfbRect) * c.hudN); } // (GXV_EDGE_AA; likewise)
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
    if (__builtin_expect(g_warmScope, 0)) clear = false; // (dive27 GXV_REHEARSE: a rehearsal's capture is a copy only; it must never clear the picture it is made in)
    J3n2Flush(); // (GXV_J3DNATIVE=4 / 5: the register file real before it is read here)
    CopyTexCmd c;
    c.transient = g_transientNext; // (GXV_INDCAP_SKIP: the marker covers this one copy)
    g_transientNext = false;
    c.silhHint = g_silhHintNext; // (GXV_SILH_GATE: likewise)
    g_silhHintNext = 0;
    c.bracket = g_bb.capNext ? static_cast<uint8_t>(g_bb.cur) : 0; // (GXV_BLOOM_BRACKET: the marker covers this one copy)
    g_bb.capNext = false;
    c.depthMark = g_depthMarkNext ? 1 : 0; // (hrw41: likewise; the mark stays up to the picture's end)
    if (c.silhHint != 0) { // (GXV_SILH_OCC, across26: the game's "cannot show" stands only while the occlusion query agrees; else the picture is treated as "may show")
        const int om = SilhOccMode();
        if (om != 0) {
            silhocc::History& oh = SilhOccHistory();
            const int raw = c.silhHint;
            bool down = false;
            if (raw == 1) {
                const bool ok = oh.Allows(g_frame, SilhOccParams()); // (counts the decision by reason)
                if (!ok && om != 3) { c.silhHint = 2; down = true; }
            }
            oh.NoteFrame(g_frame, raw, down);
        }
    }
    c.src = g.texCopySrc;
    c.dstW = g.texCopyDstW ? g.texCopyDstW : static_cast<uint16_t>(g.texCopySrc.w);
    c.dstH = g.texCopyDstH ? g.texCopyDstH : static_cast<uint16_t>(g.texCopySrc.h);
    const uint32_t fmt = g.texCopyFmt;
    RecordCrowdCopy(c, reinterpret_cast<uintptr_t>(dest), fmt);
    c.gxFmt = static_cast<uint8_t>(fmt); // (gpu27: the scenes25 census names the format and the buffer)
    c.gxDest = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(dest));
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
    GeoOcclCopy(c.clear.enable && c.clear.depth); // (GXV_OCCLCULL: a depth clear ends the occluders' epoch; GXV_GEOPASS: the copy's phase)
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
    J3n2Flush(); // (GXV_J3DNATIVE=4 / 5: the register file real before it is read here)
    sc::PictureEnd(); // (GXV_STREAM_CENSUS, vgx_census.inc: a counted picture ends here; one flag test otherwise)
    g_transientNext = false; // (GXV_INDCAP_SKIP: a marker never reaches past the picture's display copy)
    g_silhHintNext = 0;
    CopyDispCmd c;
    c.src = g.dispCopySrc;
    c.clear = MakeClear(clear);
    if (c.clear.enable && c.clear.depth) OcclBreak(); // (GXV_OCCLCULL)
    stats::CopyFormat(0, true);
    if (TraceFrame())
        std::fprintf(stderr, "[trace] COPYDISP src=(%d,%d,%d,%d) clear=%d color=(%.2f,%.2f,%.2f,%.2f) z=%.3f masks c%d a%d z%d\n", c.src.x, c.src.y, c.src.w,
                     c.src.h, clear, c.clear.rgba[0], c.clear.rgba[1], c.clear.rgba[2], c.clear.rgba[3], c.clear.z, c.clear.color, c.clear.alpha, c.clear.depth);
    if (ChaOn()) CharaFinish(c);
    if (HudNativeMode() || HrwMode()) { g_cha.hudDepth = 0; g_hudnDepth = 0; } // (GXV_HUD_NATIVE, GXV_HRW: a picture's markers never reach into the next one)
    g_ownerKey = 0; // (GXV_HRW)
    g_playerMarkDepth = 0; // (g30: a picture's marker never reaches into the next one)
    g_depthMarkNext = false; // (hrw41: a picture's mark never reaches into the next one)
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
    const bool on = VGX_KNOB(EnvInt("GXV_DEADCOPY", 1) != 0) != 0;
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

// ---- GXV_INDCAP_SKIP (gpu23): the game's "Indirect" screen capture left out when no draw of its picture reads it
// CaptureScreenDirector captures the whole EFB as RGB565 every picture at DrawType_CaptureScreenIndirect while no other capture
// timing is asked for (GameScene::draw3D, after the effects), for the indirect models, water, haze and IndDummy particles and
// layouts that are drawn after it in the same picture. Where none of them is on screen (Good Egg, Honeyhive's meadows, most of the
// Observatory) the copy is read by nothing, yet on the console it splits the EFB's scene (EFB stored, a copy scene or an
// EFBSWAP2 hand-over plus its full-screen draw-back quad, EFB loaded again). The game marks this one copy
// (PORT_GX_MARK_TRANSIENT_COPY, decomp CaptureScreenDirector::captureIfAllow, only for the "Indirect" timing with no request
// pending); here such a copy is left out (skip: only its clear, it has none) when no later draw or atlas copy of this picture
// samples its texture before the texture is written again.
// Why exact: every reader of an "Indirect" capture comes after it in the same picture (draw3D's executeDrawAfterIndirect, the
// effects' indirect pass, draw2D's IndDummy panes); the next picture that draws the 3D scene captures again before its readers;
// the pause freeze, the screen preserver, the wipes and the camera cover ask for a capture timing of their own and capture
// themselves before they read (then the marker is not given). A left-out capture leaves the texture with an older picture that
// nothing looks at. The rule is checked on every picture: a draw that samples a texture whose last write was a left-out capture,
// before the texture is written again, is counted ("stale reads"; it must stay 0) and named in the log.
//   GXV_INDCAP_SKIP=0 (default) off: the marker is not even written; 1 on; 2 verify only: nothing is left out, the counts and the
//   stale-read check are made as if it were (run =2 first on a tour; =1 when "stale reads 0").
// GXV_INDCAP_HALFRATE (a MINOR CHANGE, Performance mode): a marked capture that HAS readers is also left out on the second of two
// pictures when the previous picture made it for real: the water, haze and indirect models then show the scene behind them as the
// previous picture captured it (one picture late, 16.7 ms at 60). Never two pictures in a row, never after a picture that left it
// out. 0 (default) off; 1 on in every mode; 2 on only with GXV_AUTOQ_TARGET=60 (Performance). Exact pictures need it at 0.
// Log (basic log, once a minute while on): "indcap: <s> s, N pictures, M marked captures: left out L (2: would be), kept K for a
// reader (H of those left out: GXV_INDCAP_HALFRATE); stale reads S".
int IndCapHalfRate() {
    // (stagepreset25: stage-settable, port/stage_knob.h)
    const int m = static_cast<int>(PORT_STAGE_KNOB(int32_t, [] {
        const long v = port::stageknob::Int("GXV_INDCAP_HALFRATE", 0);
        if (v == 2) return EnvInt("GXV_AUTOQ_TARGET", 0) == 60 ? 1 : 0;
        return v == 1 ? 1 : 0;
    }()));
    return m;
}
// GXV_SCENES25_DRY=<mask> (scenes25, default 0; class E under GXV_INDCAP_SKIP's rules): captures with NO reader in their picture are left out
// by the Indirect capture's rule (MarkTransientCopies: no later draw or atlas copy samples the texture before it is written again; PREDICT /
// STAGE / WARMUP keep the capture where an earlier picture's reader was seen; "stale reads 0" must hold), for the classes of capture the mask
// names (vgx_cmd.h CopyShapeClass): 1 the player's drop-shadow corner (CC_CORNER), 2 whole-EFB 1:1 captures other than the Indirect one
// (CC_SHADOWA, CC_COLWHOLE), 4 tiles and box-halved captures (CC_CLIPMASK, CC_ATILE, CC_COLHALF, CC_COLTILE). Alone it implies
// GXV_INDCAP_SKIP=1 for these copies (the game's own Indirect marker stays off unless GXV_INDCAP_SKIP is set); with GXV_INDCAP_SKIP=2 the
// would-be counts are made and nothing is left out. The census (GXV_SCENES25=1) says per class how many captures have no reader.
int Scenes25Dry() {
    const int m = static_cast<int>(PORT_STAGE_KNOB(int32_t, std::clamp<long>(port::stageknob::Int("GXV_SCENES25_DRY", 0), 0, 7)));
    return m;
}
// GXV_INDCAP_VIS (indvis38, default 0; class E): 1 apply, 2 count only. The Indirect capture is also left out when it HAS readers but every one is invisible (an empty scissor / extent;
// vgx_indcap_vis.h has the rules and the refusals). Alone it implies the marker (mode 3: no unread capture is left out). With GXV_INDCAP_SKIP=2 nothing is left out either.
// Log, every 10 s while on: "indcap-vis: ..." (dry pictures counted / applied / refused by reason).
int IndCapVis() {
    return static_cast<int>(PORT_STAGE_KNOB(int32_t, std::clamp<long>(port::stageknob::Int("GXV_INDCAP_VIS", 0), 0, 2)));
}
// GXV_INDCAP_DECAY_CENSUS=<N> (ind24, default 0, COUNT ONLY, no picture changes): runs the "decay" rule beside GXV_INDCAP_SKIP's stage latch: a capture the stage latch alone
// holds back (no reader of an earlier picture's capture for N pictures, not in the PREDICT window, past warm-up) is counted as "would be left out", and a later reader of an
// earlier picture's capture that would then have found an old texture is counted as a would-be stale read (with its age). Alone it implies GXV_INDCAP_SKIP=2 (nothing left out).
// A decay is NOT exact: a reader that first appears after the window cannot be known beforehand (ind24 report). Log: "indcap-decay:" in the minute block.
long DecayCensusN() { return static_cast<long>(PORT_STAGE_KNOB(int32_t, std::clamp<long>(port::stageknob::Int("GXV_INDCAP_DECAY_CENSUS", 0), 0, 1000000))); }
int IndCapSkipMode() {
    const int m = static_cast<int>(PORT_STAGE_KNOB(int32_t, [] {
        const int skip = static_cast<int>(std::clamp<long>(port::stageknob::Int("GXV_INDCAP_SKIP", 0), 0, 2));
        return skip ? skip : ((IndCapHalfRate() || IndCapVis()) ? 3 : (Scenes25Dry() ? 1 : ((DecayCensusN() > 0 || vgx::indcappeek::Mode() > 0) ? 2 : 0))); // (GXV_INDCAP_PEEK: the marker, nothing left out here; 3: the half rate / GXV_INDCAP_VIS alone; no unread capture is left out)
    }()));
    return m;
}
// gpu23 round 2 (console, Honeyhive 1 start: "stale reads 15" in the first minute). Not every reader comes after the capture:
// SMG also has readers that sample the screen texture BEFORE the picture's own capture, i.e. the PREVIOUS picture's capture:
// models whose material has the "IndDummy" texture (DisplayListMaker::checkTexture puts the screen texture there, whatever draw
// buffer the model is in: crystals, glass, the opaque and translucent lists all come before DrawType_CaptureScreenIndirect),
// particles with the IndDummy texture (EffectSystem::entry) drawn by an effect group before it (EffectDraw3D), Mario's model
// display list with the screen texture (J3DModelX), and any picture that reads it without capturing (a capture timing of its
// own pending). When the capture of picture N had no reader after it, it was left out, and such a reader in picture N+1 got the
// capture of N-1 or older. The front end cannot see picture N+1 when it decides about N (it hands N to the back end first), so
// the rule now keeps the capture whenever the picture's texture has such readers:
//  - PREDICT: a draw that samples the texture before this picture writes it (a reader of the previous picture's capture) keeps
//    this picture's capture and the next GXV_INDCAP_PREDICT pictures' (default 120, 2 s at 60): a refracting object or a haze
//    particle stays on screen for many pictures, and each of them reads its predecessor's capture. This removes every stale read
//    of a reader that was already on screen in the picture before.
//  - STAGE: the game tags the marker with its stage (a 16-bit hash of the stage name and scenario, CaptureScreenDirector). Once a
//    reader of an earlier picture's capture (or a stale read) is seen in a stage, no capture is left out for the rest of that
//    stage, and the stage is remembered for the rest of the run: on later visits it never leaves one out. So a stage with such
//    readers costs at most the pictures before the first one is seen on the first visit, counted and named as "STALE READ"
//    (with its class: before the picture's own capture or in a picture without one; a J3D model's material or streamed
//    vertices; the shader id).
//  - WARMUP: a stage leaves no capture out before GXV_INDCAP_WARMUP pictures (default 600, 10 s at 60) with captures and without
//    any such reader have passed in it (counted from the stage's first marker): the console's stale reads (Honeyhive 1) all came
//    in the first minute, around the stage's start and its opening camera, where readers of that kind show up first.
//  An exact rule for that first picture would need the next picture's commands before this one is drawn (one picture more of
//  latency between the front end and the back end); not done.
//   GXV_INDCAP_PREDICT=<pictures> (0: off), GXV_INDCAP_STAGE=1 (default; 0: off), GXV_INDCAP_WARMUP=<pictures> (0: off). All three
//   off: the round-1 rule.
//   GXV_INDCAP_EARLY=<tag>[,<tag>...]: stages known to have such readers, from earlier runs' logs ("indcap: stage <tag> ...").
namespace {
struct IcCount { uint64_t pictures = 0, marked = 0, left = 0, kept = 0, half = 0, stale = 0, held = 0, pre = 0, refreshed = 0, warmKept = 0, earlyKept = 0, hrwFresh = 0; };
IcCount g_icCtr, g_icTotal;
// (GXV_INDCAP_DECAY_CENSUS) state and totals of the would-be decay
struct DcStale { uint32_t texId; uint64_t since; }; // since: the picture of the first would-be left-out capture after the last real one
std::vector<DcStale> g_dcStale;
uint64_t g_dcLastRd = 0, g_dcWould = 0, g_dcKeptLatch = 0, g_dcStaleReads = 0, g_dcMaxAge = 0, g_dcAgeSum = 0;
uint32_t g_dcStage = 0xFFFFFFFFu;
void DcClear(uint32_t texId) {
    for (size_t k = 0; k < g_dcStale.size(); ++k)
        if (g_dcStale[k].texId == texId) { g_dcStale[k] = g_dcStale.back(); g_dcStale.pop_back(); return; }
}
void DcReader(uint32_t texId) { // a reader of an earlier picture's capture of texId (loop 1 of MarkTransientCopies)
    g_dcLastRd = g_frame;
    for (size_t k = 0; k < g_dcStale.size(); ++k)
        if (g_dcStale[k].texId == texId) {
            const uint64_t age = g_frame - g_dcStale[k].since + 1; // pictures between the texture's last real capture and the one this reader wanted
            ++g_dcStaleReads; g_dcAgeSum += age; g_dcMaxAge = std::max(g_dcMaxAge, age);
            DcClear(texId);
            return;
        }
}
uint64_t g_ivSeen = 0, g_ivDry = 0, g_ivApplied = 0, g_ivRefused[vgx::indcapvis::V_COUNT] = {}; // (GXV_INDCAP_VIS: captures with readers / dry / left out / refused by reason)
void IvLog() {
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::seconds(10) || !g_ivSeen) return;
    last = now;
    char line[640];
    std::snprintf(line, sizeof line,
                  "indcap-vis: GXV_INDCAP_VIS=%d: %llu captures with readers, dry (no visible reader) %llu, left out %llu; refused: visible %llu, atlas %llu, occlusion %llu, held %llu, "
                  "hrw-pair %llu, camera-cut %llu, shape %llu, prevcap %llu",
                  IndCapVis(), static_cast<unsigned long long>(g_ivSeen), static_cast<unsigned long long>(g_ivDry), static_cast<unsigned long long>(g_ivApplied),
                  static_cast<unsigned long long>(g_ivRefused[vgx::indcapvis::V_VISIBLE]), static_cast<unsigned long long>(g_ivRefused[vgx::indcapvis::V_ATLAS]),
                  static_cast<unsigned long long>(g_ivRefused[vgx::indcapvis::V_OCC]), static_cast<unsigned long long>(g_ivRefused[vgx::indcapvis::V_HELD]),
                  static_cast<unsigned long long>(g_ivRefused[vgx::indcapvis::V_PAIR]), static_cast<unsigned long long>(g_ivRefused[vgx::indcapvis::V_CUT]),
                  static_cast<unsigned long long>(g_ivRefused[vgx::indcapvis::V_SHAPE]), static_cast<unsigned long long>(g_ivRefused[vgx::indcapvis::V_PREVCAP]));
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
}
// GXV_HRW_CAPFIX (default 1, a bug fix; 0 = the old behaviour): the half rate counts PICTURES THE FRONT END MADE (g_frame: real A pictures only; HRW's second picture of a pair takes no
// frame number), so with GXV_HRW pairs "the previous picture" is 33.3 ms back and the capture alternated between 33 ms and 67 ms old (a 15 Hz misregistration of the water /
// refraction while the camera moves). While HRW is pairing (this A opened a pair, or the last A did) a capture that the half rate would leave out is made for real: the readers
// then never see a capture older than the pair before them. HRW off: g_buf.hrwId is never set, so nothing here changes.
bool HrwCapFixOn() { return VGX_KNOB(EnvInt("GXV_HRW_CAPFIX", 1) != 0 ? 1 : 0) != 0; }
hrwcapfix::Tracker g_icPair; // (vgx_hrw_capfix.h) the last picture (g_frame) that opened an HRW pair
extern "C" int port_hrw_indcap_exact(void) __attribute__((weak));  // (gov49, vgx_galaxy.cpp: 1 while the 60 Assist governor has not proven plain 60 solid: the water's capture stays exact)
bool IcHrwPairing() { // (once or more a picture, after the game's pair marker: g_buf.hrwId is set from the picture's first command)
    const bool pairing = g_icPair.Pairing(g_frame, g_buf.hrwId != 0, HrwCapFixOn());
    return pairing || (HrwCapFixOn() && port_hrw_indcap_exact && port_hrw_indcap_exact() != 0);   // (gov49 GXV_HRW_GOV5_EXACT)
}
uint64_t g_icCutHeld = 0; // (GXV_INDCAP_CAMGATE: halved pictures refused at / just after a camera cut, all pictures)
struct IcStale { uint32_t texId; uint64_t frame; }; // (frame: the picture that left the capture out)
std::vector<IcStale> g_icStale; // textures whose last write was a left-out (=2: would-be left out) capture
struct IcMade { uint32_t texId; uint64_t frame; };
std::vector<IcMade> g_icMade; // (GXV_INDCAP_HALFRATE) the picture each marked texture was last really captured in
struct IcHold { uint32_t texId; uint64_t until; };
std::vector<IcHold> g_icHold;  // (PREDICT) marked textures whose captures are made for real until that picture
std::vector<uint32_t> g_icTargets; // textures marked captures were made into (the readers of earlier pictures are looked for)
// (GXV_INDCAP_CAMGATE=1, class U: heavyopt25) the half-rate capture is never taken over a camera cut: the picture in which the game's stage camera
// jumped (port_camera_cut_count changed since the last picture that asked) makes its capture for real, so the haze / water / refraction never
// shows the scene from BEFORE a cut (a one-picture ghost at the cut), and a halved picture is also refused while a cut is within GXV_INDCAP_CAMGATE_PICS
// pictures (default 2) of the last real capture. Counted in the "indcap:" line. 0 (default) = the half rate as before.
extern "C" uint32_t port_camera_cut_count(void) __attribute__((weak));
bool IcCamCut() {
    if (!PORT_STAGE_KNOB(int32_t, port::stageknob::Int("GXV_INDCAP_CAMGATE", 0) != 0 ? 1 : 0)) return false;
    static uint32_t seen = 0; static uint64_t lastCutFrame = 0, frameSeen = 0;
    if (frameSeen != g_frame) { // (once a picture: the first marked copy asks)
        frameSeen = g_frame;
        const uint32_t cuts = port_camera_cut_count ? port_camera_cut_count() : 0;
        if (cuts != seen) { seen = cuts; lastCutFrame = g_frame; }
    }
    const long pics = static_cast<long>(VGX_KNOB(static_cast<int32_t>(std::clamp<long>(EnvInt("GXV_INDCAP_CAMGATE_PICS", 2), 0, 600))));
    const bool hold = lastCutFrame != 0 && g_frame < lastCutFrame + static_cast<uint64_t>(pics) + 1;
    if (hold) ++g_icCutHeld;
    return hold;
}
// GXV_INDCAP_REFRESH=<pictures> (pixcuts25, default 0 = off; class U, only ever makes MORE captures): a marked capture nothing reads is still made when the
// texture's last real capture is this many pictures old. What it bounds: the one picture in which a reader of an earlier picture's capture first shows up in a
// stage (a refracting object or haze that comes into view; the rule cannot see a picture before it is drawn) samples the texture's last real capture, which
// without this can be arbitrarily old (every picture since the warm-up left it out); with it the stale picture is at most this old. =30: at most half a second
// old, and one capture in 30 pictures kept of the ones left out (3 %). The exact rule is unchanged: it stays the default.
long IcRefresh() { return static_cast<long>(PORT_STAGE_KNOB(int32_t, std::clamp<long>(port::stageknob::Int("GXV_INDCAP_REFRESH", 0), 0, 100000))); }
struct IcReal { uint32_t texId; uint64_t frame; };
std::vector<IcReal> g_icReal; // the picture each marked texture was last really captured in (GXV_INDCAP_REFRESH)
uint64_t IcLastReal(uint32_t texId) {
    for (const IcReal& r : g_icReal)
        if (r.texId == texId) return r.frame;
    return 0;
}
void IcNoteReal(uint32_t texId) {
    for (IcReal& r : g_icReal)
        if (r.texId == texId) { r.frame = g_frame; return; }
    if (g_icReal.size() < 16) g_icReal.push_back(IcReal{texId, g_frame});
}
long IcPredict() { return static_cast<long>(VGX_KNOB(static_cast<int32_t>(std::clamp<long>(EnvInt("GXV_INDCAP_PREDICT", 120), 0, 1000000)))); }
long IcWarmup() { return static_cast<long>(VGX_KNOB(static_cast<int32_t>(std::clamp<long>(EnvInt("GXV_INDCAP_WARMUP", 600), 0, 100000000)))); }
uint32_t g_icStageCur = 0xFFFFFFFFu; // (WARMUP: the stage being counted, and its clean pictures so far)
uint64_t g_icClean = 0;
bool IcStageOn() { return VGX_KNOB(EnvInt("GXV_INDCAP_STAGE", 1) != 0) != 0; }
std::vector<uint32_t> g_icEarly; // stages (tags) with readers of an earlier picture's capture: none of their captures is left out
bool g_icEarlyEnvRead = false;
bool IcEarlyStage(uint32_t stage) {
    if (!g_icEarlyEnvRead) {
        g_icEarlyEnvRead = true;
        for (const char* q = diag::Env("GXV_INDCAP_EARLY"); q && *q;) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(q, &end, 0);
            if (end == q) break;
            g_icEarly.push_back(static_cast<uint32_t>(v));
            q = *end == ',' ? end + 1 : end;
        }
    }
    return IcStageOn() && std::find(g_icEarly.begin(), g_icEarly.end(), stage) != g_icEarly.end();
}
void IcNoteEarly(uint32_t stage, const char* why) {
    if (!IcStageOn() || std::find(g_icEarly.begin(), g_icEarly.end(), stage) != g_icEarly.end()) return;
    if (g_icEarly.size() < 256) g_icEarly.push_back(stage);
    if (port_log_summary)
        port_log_summary("indcap: stage %u (0x%04x) has readers of an earlier picture's capture (%s): its captures are all made from now on "
                         "(GXV_INDCAP_EARLY=%u skips the first sight next run)", static_cast<unsigned>(stage), static_cast<unsigned>(stage), why,
                         static_cast<unsigned>(stage));
}
void IcHoldTex(uint32_t texId, long pictures) {
    if (pictures <= 0) return;
    const uint64_t until = g_frame + static_cast<uint64_t>(pictures);
    for (IcHold& h : g_icHold)
        if (h.texId == texId) { h.until = std::max(h.until, until); return; }
    if (g_icHold.size() < 16) g_icHold.push_back(IcHold{texId, until});
}
bool IcHeld(uint32_t texId) {
    for (const IcHold& h : g_icHold)
        if (h.texId == texId) return g_frame < h.until;
    return false;
}
void IcLog(int mode) {
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::seconds(60) || !g_icCtr.pictures) return;
    const double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    const IcCount& c = g_icCtr;
    char line[720];
    std::snprintf(line, sizeof line,
                  "indcap: %.0f s, %llu pictures, %llu marked captures: left out %llu%s, kept %llu for a reader (%llu of those left out: "
                  "GXV_INDCAP_HALFRATE), %llu kept for readers of the previous picture's (GXV_INDCAP_PREDICT / _STAGE; %llu such reads); "
                  "stale reads %llu (total %llu); GXV_INDCAP_CAMGATE: %llu halved pictures refused near a camera cut; "
                  "GXV_HRW_CAPFIX=%d: %llu captures made fresh because HRW is pairing (total %llu)",
                  secs, static_cast<unsigned long long>(c.pictures), static_cast<unsigned long long>(c.marked), static_cast<unsigned long long>(c.left),
                  mode == 2 ? " (GXV_INDCAP_SKIP=2: would be)" : "", static_cast<unsigned long long>(c.kept), static_cast<unsigned long long>(c.half),
                  static_cast<unsigned long long>(c.held), static_cast<unsigned long long>(c.pre), static_cast<unsigned long long>(c.stale),
                  static_cast<unsigned long long>(g_icTotal.stale + c.stale), static_cast<unsigned long long>(g_icCutHeld),
                  HrwCapFixOn() ? 1 : 0, static_cast<unsigned long long>(c.hrwFresh), static_cast<unsigned long long>(g_icTotal.hrwFresh + c.hrwFresh));
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    {
        // (pixcuts25) the run's totals, one line a minute: the line to read at the end of a tour. "stale reads" must stay 0 after the first sight of a stage's
        // readers (each stage with such readers is named once by "indcap: stage ... has readers"; "stages learned" counts them).
        char run[420];
        std::snprintf(run, sizeof run,
                      "indcap-run: GXV_INDCAP_SKIP=%d, run totals: %llu pictures, %llu marked captures: left out %llu, kept %llu (for a reader of this picture), "
                      "held back %llu (earlier-picture readers seen %llu, warm-up %llu, stage learned %llu), refreshed %llu (GXV_INDCAP_REFRESH=%ld); STALE READS %llu; stages learned %u",
                      mode, static_cast<unsigned long long>(g_icTotal.pictures + c.pictures), static_cast<unsigned long long>(g_icTotal.marked + c.marked),
                      static_cast<unsigned long long>(g_icTotal.left + c.left), static_cast<unsigned long long>(g_icTotal.kept + c.kept),
                      static_cast<unsigned long long>(g_icTotal.held + c.held), static_cast<unsigned long long>(g_icTotal.pre + c.pre),
                      static_cast<unsigned long long>(g_icTotal.warmKept + c.warmKept), static_cast<unsigned long long>(g_icTotal.earlyKept + c.earlyKept),
                      static_cast<unsigned long long>(g_icTotal.refreshed + c.refreshed), IcRefresh(), static_cast<unsigned long long>(g_icTotal.stale + c.stale),
                      static_cast<unsigned>(g_icEarly.size()));
        if (port_log_summary) port_log_summary("%s", run);
        else std::fprintf(stderr, "[vgx] %s\n", run);
    }
    if (const long dn = DecayCensusN(); dn > 0) {
        char dl[360];
        std::snprintf(dl, sizeof dl,
                      "indcap-decay: GXV_INDCAP_DECAY_CENSUS=%ld (count only, nothing left out): run totals: %llu captures a decay would leave out, %llu kept (reader within %ld pictures); "
                      "would-be STALE READS %llu (age in pictures: max %llu, mean %llu)",
                      dn, static_cast<unsigned long long>(g_dcWould), static_cast<unsigned long long>(g_dcKeptLatch), dn, static_cast<unsigned long long>(g_dcStaleReads),
                      static_cast<unsigned long long>(g_dcMaxAge), static_cast<unsigned long long>(g_dcStaleReads ? g_dcAgeSum / g_dcStaleReads : 0));
        if (port_log_summary) port_log_summary("%s", dl);
        else std::fprintf(stderr, "[vgx] %s\n", dl);
    }
    g_icTotal.refreshed += c.refreshed; g_icTotal.warmKept += c.warmKept; g_icTotal.earlyKept += c.earlyKept;
    g_icTotal.pictures += c.pictures; g_icTotal.marked += c.marked; g_icTotal.left += c.left; g_icTotal.kept += c.kept; g_icTotal.half += c.half;
    g_icTotal.stale += c.stale; g_icTotal.held += c.held; g_icTotal.pre += c.pre; g_icTotal.hrwFresh += c.hrwFresh;
    g_icCtr = IcCount{};
}
} // namespace
void IndCapCounters(uint64_t out[8]) {
    out[0] = g_icTotal.pictures + g_icCtr.pictures;
    out[1] = g_icTotal.marked + g_icCtr.marked;
    out[2] = g_icTotal.left + g_icCtr.left;
    out[3] = g_icTotal.kept + g_icCtr.kept;
    out[4] = g_icTotal.half + g_icCtr.half;
    out[5] = g_icTotal.stale + g_icCtr.stale;
    out[6] = g_icTotal.held + g_icCtr.held;
    out[7] = g_icTotal.pre + g_icCtr.pre;
}
void IndCapRunCounters(uint64_t out[4]) {
    out[0] = g_icTotal.refreshed + g_icCtr.refreshed;
    out[1] = g_icTotal.warmKept + g_icCtr.warmKept;
    out[2] = g_icTotal.earlyKept + g_icCtr.earlyKept;
    out[3] = g_icEarly.size();
}
static void MarkTransientCopies() {
    const int mode = IndCapSkipMode();
    if (!mode) return;
    ++g_icCtr.pictures;
    const size_t n = g_buf.cmds.size();
    if (const int dry = Scenes25Dry()) { // (GXV_SCENES25_DRY: copies of the named shapes are handled like the marked capture, and left out when nothing reads them)
        for (CopyTexCmd& c : g_buf.copyTex) {
            if (c.transient || c.skip || c.fill || c.texId == 0) continue;
            const CopyClass k = CopyShapeClass(c, g.efbW, g.efbH);
            const int bit = k == CC_CORNER ? 1 : (k == CC_SHADOWA || k == CC_COLWHOLE) ? 2 : (k == CC_CLIPMASK || k == CC_ATILE || k == CC_COLHALF || k == CC_COLTILE) ? 4 : 0;
            if (bit & dry) { c.transient = true; c.dry = true; }
        }
    }
    const uint32_t stage = g_transientStage; // (the stage tag of this picture's marker, or the last one seen)
    if (stage != g_icStageCur) { g_icStageCur = stage; g_icClean = 0; }
    if (stage != g_dcStage) { g_dcStage = stage; g_dcLastRd = g_frame; g_dcStale.clear(); } // (DECAY_CENSUS: a new stage starts its own window)
    const long dcN = DecayCensusN();
    const uint64_t pre0 = g_icCtr.pre;
    const bool hrwPair = IcHrwPairing(); // (GXV_HRW_CAPFIX: this A opens a pair, or the last one did; its second picture takes no frame number)
    auto staleAt = [&](uint32_t id) -> int {
        for (size_t k = 0; k < g_icStale.size(); ++k)
            if (g_icStale[k].texId == id) return static_cast<int>(k);
        return -1;
    };
    auto unstale = [&](int k) { g_icStale[k] = g_icStale.back(); g_icStale.pop_back(); };
    for (const CopyTexCmd& c : g_buf.copyTex) // (the targets: learned from the marked copies themselves)
        if (c.transient && c.texId && std::find(g_icTargets.begin(), g_icTargets.end(), c.texId) == g_icTargets.end()) {
            if (g_icTargets.size() >= 8) g_icTargets.erase(g_icTargets.begin());
            g_icTargets.push_back(c.texId);
        }
    // (1) the readers of an earlier picture's capture: a draw that samples a marked texture before this picture writes it. Each
    // one keeps the captures for GXV_INDCAP_PREDICT pictures; one whose texture's last capture was left out is a stale read.
    if (!g_icTargets.empty()) {
        uint32_t open = (1u << g_icTargets.size()) - 1u; // (targets not written yet in this picture)
        bool marked[8] = {};
        for (size_t i = 0; i < n; ++i) // (does the picture capture each target at all - for the log's class)
            if (g_buf.cmds[i].type == CMD_COPY_TEX) {
                const CopyTexCmd& y = g_buf.copyTex[g_buf.cmds[i].arg];
                for (size_t t = 0; t < g_icTargets.size(); ++t) marked[t] = marked[t] || (y.transient && y.texId == g_icTargets[t]);
            }
        auto target = [&](uint32_t id) -> int {
            for (size_t t = 0; t < g_icTargets.size(); ++t)
                if ((open & (1u << t)) && g_icTargets[t] == id) return static_cast<int>(t);
            return -1;
        };
        for (size_t i = 0; i < n && open; ++i) {
            const Cmd& x = g_buf.cmds[i];
            if (x.type == CMD_DRAW) {
                const DrawRec& d = g_buf.draws[x.arg];
                for (int m = 0; m < 8; ++m) {
                    if (!(d.texMask & (1u << m))) continue;
                    const int t = target(d.tex[m].texId);
                    if (t < 0) continue;
                    ++g_icCtr.pre;
                    DcReader(d.tex[m].texId);
                    IcHoldTex(d.tex[m].texId, IcPredict());
                    IcNoteEarly(stage, d.vbuf ? "a J3D model's material before the capture" : "streamed vertices before the capture");
                    const int k = staleAt(d.tex[m].texId);
                    if (k < 0) continue;
                    if (g_icCtr.stale + g_icTotal.stale < 16 && port_log_summary)
                        port_log_summary("indcap: STALE READ: picture %llu (%u after the one that left the capture out), stage %u: draw %u of %u "
                                         "(fs %u, texture slot %d, %s) samples texture %u %s (GXV_INDCAP_SKIP=%d)",
                                         static_cast<unsigned long long>(g_frame), static_cast<unsigned>(g_frame - g_icStale[k].frame),
                                         static_cast<unsigned>(stage), static_cast<unsigned>(i), static_cast<unsigned>(n), static_cast<unsigned>(d.fsId), m,
                                         d.vbuf ? "a cached display list: a J3D model's material" : "streamed vertices: a particle, a 2D or direct draw",
                                         d.tex[m].texId,
                                         marked[t] ? "before this picture's own capture of it" : "in a picture that does not capture it (another timing)",
                                         mode);
                    ++g_icCtr.stale;
                    unstale(k);
                }
            } else if (x.type == CMD_COPY_TEX) {
                const CopyTexCmd& y = g_buf.copyTex[x.arg];
                int t = y.srcTex ? target(y.srcTex) : -1;
                if (t >= 0) {
                    ++g_icCtr.pre;
                    DcReader(y.srcTex);
                    IcHoldTex(y.srcTex, IcPredict());
                    IcNoteEarly(stage, "an atlas copy before the capture");
                    const int k = staleAt(y.srcTex);
                    if (k >= 0) { ++g_icCtr.stale; unstale(k); }
                }
                t = target(y.texId);
                if (t >= 0) open &= ~(1u << t);
                const int k = staleAt(y.texId);
                if (k >= 0) unstale(k); // (written: a marked copy left out below puts it back)
                DcClear(y.texId);
            } else if (x.type == CMD_TEX_FREE || x.type == CMD_TEX_UPLOAD) {
                const uint32_t id = x.type == CMD_TEX_FREE ? x.arg : g_buf.uploads[x.arg].texId;
                const int t = target(id);
                if (t >= 0) open &= ~(1u << t);
                const int k = staleAt(id);
                if (k >= 0) unstale(k);
                DcClear(id);
            }
        }
    }
    // (stale textures not read in this picture stay listed; ones written by something other than a marked copy left the list above)
    if (g_icCtr.pre == pre0) ++g_icClean;
    const bool warm = g_icClean > static_cast<uint64_t>(IcWarmup()); // (this picture counted: WARMUP pictures before it were clean)
    // (2) this picture's marked captures
    for (size_t i = 0; i < n; ++i) {
        if (g_buf.cmds[i].type != CMD_COPY_TEX) continue;
        CopyTexCmd& c = g_buf.copyTex[g_buf.cmds[i].arg];
        if (!c.transient || c.skip) continue;
        ++g_icCtr.marked;
        if (c.fromPass || c.passMore || c.srcTex || c.dstTexW || c.depth || c.texId == 0) { ++g_icCtr.kept; continue; } // (not the plain capture)
        bool read = false;
        for (size_t j = i + 1; j < n && !read; ++j) {
            const Cmd& x = g_buf.cmds[j];
            if (x.type == CMD_DRAW) {
                const DrawRec& d = g_buf.draws[x.arg];
                for (int m = 0; m < 8 && !read; ++m) read = (d.texMask & (1u << m)) && d.tex[m].texId == c.texId;
            } else if (x.type == CMD_COPY_TEX) {
                const CopyTexCmd& y = g_buf.copyTex[x.arg];
                if (y.srcTex == c.texId) read = true;
                else if (y.texId == c.texId && !y.skip) break; // (written again: what follows reads that copy)
            } else if ((x.type == CMD_TEX_FREE && x.arg == c.texId) || (x.type == CMD_TEX_UPLOAD && g_buf.uploads[x.arg].texId == c.texId)) {
                break;
            }
        }
        if (read && IndCapVis()) {
            // (GXV_INDCAP_VIS: every reader of this capture invisible -> left out like an unread one; the same bookkeeping)
            std::vector<vgx::indcapvis::Reader> rd;
            for (size_t j = i + 1; j < n; ++j) {
                const Cmd& x = g_buf.cmds[j];
                if (x.type == CMD_DRAW) {
                    const DrawRec& d = g_buf.draws[x.arg];
                    for (int m = 0; m < 8; ++m)
                        if ((d.texMask & (1u << m)) && d.tex[m].texId == c.texId) {
                            vgx::indcapvis::Reader r;
                            r.occ = d.occ != 0;
                            r.exX = d.exX; r.exY = d.exY; r.exW = d.exW; r.exH = d.exH;
                            r.scX = d.rs.scX; r.scY = d.rs.scY; r.scW = d.rs.scW; r.scH = d.rs.scH;
                            rd.push_back(r);
                            break;
                        }
                } else if (x.type == CMD_COPY_TEX) {
                    const CopyTexCmd& y = g_buf.copyTex[x.arg];
                    if (y.srcTex == c.texId) { vgx::indcapvis::Reader r; r.atlas = true; rd.push_back(r); }
                    else if (y.texId == c.texId && !y.skip) break;
                } else if ((x.type == CMD_TEX_FREE && x.arg == c.texId) || (x.type == CMD_TEX_UPLOAD && g_buf.uploads[x.arg].texId == c.texId)) {
                    break;
                }
            }
            vgx::indcapvis::Facts f;
            f.efbW = g.efbW; f.efbH = g.efbH;
            f.overscan = port::getenv25::OverscanOn();
            f.held = IcHeld(c.texId) || IcEarlyStage(stage) || !warm;
            // GXV_INDCAP_VIS skips only when all readers are off-screen/empty-scissor (100% exact): only guard actual HRW pairs, not GXV_HRW_GOV5_EXACT
            f.pairing = g_icPair.Pairing(g_frame, g_buf.hrwId != 0, HrwCapFixOn()) || g_buf.hrwId != 0;
            f.cut = IcCamCut();
            f.shape = c.clear.enable || c.dry;
            f.prevcap = port::getenv25::PrevCapOn();
            const vgx::indcapvis::Verdict v = vgx::indcapvis::Decide(rd.data(), static_cast<int>(rd.size()), f);
            ++g_ivSeen;
            if (v == vgx::indcapvis::V_DRY) {
                ++g_ivDry;
                if (IndCapVis() == 1 && mode != 2) {
                    ++g_ivApplied;
                    ++g_icCtr.left;
                    c.skip = true;
                    const int sk = staleAt(c.texId);
                    if (sk >= 0) g_icStale[sk].frame = g_frame;
                    else if (g_icStale.size() < 16) g_icStale.push_back(IcStale{c.texId, g_frame});
                    for (IcMade& e : g_icMade)
                        if (e.texId == c.texId) e.frame = 0; // (not made: the next picture's readers need a real capture)
                    continue;
                }
            } else {
                ++g_ivRefused[v];
            }
        }
        if (read) {
            DcClear(c.texId); // (made for real)
            ++g_icCtr.kept;
            // (GXV_INDCAP_HALFRATE: made for real by the previous picture -> left out now, its readers take that one)
            size_t k = 0;
            while (k < g_icMade.size() && g_icMade[k].texId != c.texId) ++k;
            if (!c.dry && IndCapHalfRate() && vgx::shed::g_cutGate.load(std::memory_order_relaxed) != 0 && k < g_icMade.size() && g_icMade[k].frame + 1 == g_frame && !c.clear.enable && !IcCamCut()) {
                if (!hrwcapfix::Skip(true, hrwPair)) {
                    ++g_icCtr.hrwFresh; // (GXV_HRW_CAPFIX: made for real, the old capture would be a pair or more old)
                } else {
                    ++g_icCtr.half;
                    c.skip = true;
                    continue;
                }
            }
            if (k == g_icMade.size()) {
                if (g_icMade.size() >= 16) g_icMade.erase(g_icMade.begin());
                g_icMade.push_back(IcMade{c.texId, g_frame});
            } else {
                g_icMade[k].frame = g_frame;
            }
            continue;
        }
        if (dcN > 0) { // (GXV_INDCAP_DECAY_CENSUS: only the stage latch holds this capture; would a decay of N pictures leave it out?)
            if (!IcHeld(c.texId) && warm && IcEarlyStage(stage) && g_frame - g_dcLastRd > static_cast<uint64_t>(dcN)) {
                ++g_dcWould;
                bool have = false;
                for (const DcStale& e : g_dcStale) have = have || e.texId == c.texId;
                if (!have && g_dcStale.size() < 16) g_dcStale.push_back(DcStale{c.texId, g_frame}); // (a texture already would-be stale keeps its first picture)
            } else {
                if (IcEarlyStage(stage)) ++g_dcKeptLatch;
                DcClear(c.texId); // (made for real)
            }
        }
        if (mode == 3) continue; // (the half rate alone: an unread capture is made as before)
        if (IcHeld(c.texId) || IcEarlyStage(stage) || !warm) { // (GXV_INDCAP_PREDICT / _STAGE / _WARMUP)
            ++g_icCtr.held;
            if (!warm) ++g_icCtr.warmKept;
            else if (!IcHeld(c.texId)) ++g_icCtr.earlyKept;
            continue;
        }
        if (const long refresh = IcRefresh(); refresh > 0 && g_frame >= IcLastReal(c.texId) + static_cast<uint64_t>(refresh)) { // (GXV_INDCAP_REFRESH: bounds the age of a first-sight stale read)
            ++g_icCtr.refreshed;
            continue;
        }
        ++g_icCtr.left;
        if (mode == 1) c.skip = true;
        const int k = staleAt(c.texId);
        if (k >= 0) g_icStale[k].frame = g_frame;
        else if (g_icStale.size() < 16) g_icStale.push_back(IcStale{c.texId, g_frame});
        if (mode == 1)
            for (IcMade& e : g_icMade)
                if (e.texId == c.texId) e.frame = 0; // (not made: the next picture's readers need a real capture)
    }
    for (size_t i = 0; i < n; ++i) { // (GXV_INDCAP_REFRESH: the marked captures this picture really made)
        if (g_buf.cmds[i].type != CMD_COPY_TEX) continue;
        const CopyTexCmd& c = g_buf.copyTex[g_buf.cmds[i].arg];
        if (c.transient && !c.skip && c.texId) IcNoteReal(c.texId);
    }
    if (IndCapVis()) IvLog();
    IcLog(mode);
}

// ---- GXV_PREVCAP (prevcap25): mid-picture EFB captures answered by the PREVIOUS picture, with no capture
// Each capture of the EFB in the middle of a picture ends the GXM scene and starts another that reloads colour and depth (~1.2 ms of
// pixel and ~1.3 ms of vertex time on the console). GXV_PREVCAP=<bitmask> moves classes of capture to "read the picture before":
//   bit 1  the game's "Indirect" screen capture (CaptureScreenDirector, whole EFB, RGB565; read by the indirect models, water, haze and
//          IndDummy particles after it). The capture is left out; every draw of the picture that samples its texture samples the
//          previous picture instead, as the EFB held it when that picture was shown (the back end hands the EFB's colour buffer
//          to a texture of its own at the display copy: CopyDispCmd::keepTex; no extra copy when the swap can be made).
//   bit 2  the bloom source, bit 4 others: NOT built (see agents/prevcap25/REPORT.txt: the glow's source is this picture's own glow
//          models on a black fill, or the scene the chain re-draws into; no previous picture can stand for it). Parsed and counted only.
// Why this is a MINOR CHANGE at best and visibly wrong for refraction (the report has the proof): the previous picture is the FINISHED
// one, so the pixels of an indirect model's footprint hold that model's own output (and the HUD, and the bloom glow). A reader that
// samples the screen behind itself therefore samples itself, a feedback loop with the picture's own history. The switch exists to
// measure that on a console and to carry the keeper for whatever can use it; it is not meant for a default.
// Rules (every doubt = the capture is made as before, counted): the capture must be the plain whole-EFB 1:1 RGB565 copy with no clear
// and at least one reader; no reader may read alpha (the previous picture's alpha is not 1); no atlas copy may read the texture; the
// previous picture must have asked for the keeper (consecutive pictures, same stage tag, same EFB size); no camera cut within
// GXV_PREVCAP_CAMPICS (default 2) pictures; and the back end holds no direct display (GXV_DIRECT_DISPLAY off). A picture that
// makes its capture for real after a picture that left it out points its readers BEFORE the capture at the keeper too (the texture
// is older than the previous picture then).
//   GXV_PREVCAP=0 (default) off: nothing below runs; 1 bit 1; 2/4 counted only.
//   GXV_PREVCAP_CENSUS=1: the "prevcap-census:" line (every 600 pictures): every EFB copy of the picture by class, nothing changes.
namespace {
extern "C" uint32_t port_camera_cut_count(void) __attribute__((weak));
struct PcCount {
    uint64_t pictures = 0, marked = 0, used = 0, readers = 0, pre = 0, keeps = 0, noPrev = 0, cut = 0, alpha = 0, atlas = 0, noRead = 0, shape = 0, direct = 0, skipped = 0;
};
PcCount g_pcCtr, g_pcTotal;
uint64_t g_pcKeepFrame = 0, g_pcLeftFrame = 0; // the picture that asked for the keeper; the last picture that left a capture out
uint32_t g_pcKeepStage = 0, g_pcLeftTex = 0, g_pcTexId = 0;
int g_pcKeepW = 0, g_pcKeepH = 0;
unsigned char g_pcKey[16]; // (the keeper texture's key in the copy-texture table: never a guest address)
uint32_t g_pcCutSeen = 0;
uint64_t g_pcCutFrame = 0;
// scenes28 GXV_INDPREV (look-alike for Performance): the Indirect capture (refraction / haze / heat source) is left out and its readers sample the PREVIOUS
// picture's 3D scene, copied ONCE at the end of that picture's 3D drawing (after the last draw that is not the HUD's: the HUD never appears in the source), whole
// EFB 1:1, RGB565 look (alpha 1). Not the PREVCAP keeper (that is the displayed picture, HUD included, and is refused under direct display).
//   GXV_INDPREV=0 (default) off; 1 on. GXV_INDPREV_AB=N (N>0): every N pictures alternate previous / exact (visual check). Guards, each counted: no previous picture
//   (first picture, stage change, EFB size change), camera cut within GXV_PREVCAP_CAMPICS pictures, plain-copy shape, no reader, atlas reader. TODO: zero-copy alias of the
//   flipped direct-display colour buffer instead of the copy. Not detected: a screen wipe / pause (the picture after is one picture stale for one picture).
struct IpCount { uint64_t pics = 0, used = 0, copies = 0, noPrev = 0, cut = 0, shape = 0, noRead = 0, atlas = 0, ab = 0, small = 0, skipped = 0; };
IpCount g_ipCtr;
uint64_t g_ipPic = 0;
bool IndPrevOn() { return VGX_KNOB(EnvInt("GXV_INDPREV", 0) != 0 ? 1 : 0) != 0; }
long IndPrevAb() { return static_cast<long>(VGX_KNOB(static_cast<int32_t>(std::clamp<long>(EnvInt("GXV_INDPREV_AB", 0), 0, 100000)))); }
void IpLog() {
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::seconds(5) || !g_ipCtr.pics) return;
    const double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    const IpCount& c = g_ipCtr;
    char line[600];
    std::snprintf(line, sizeof line,
                  "scenes28: GXV_INDPREV=1 AB=%ld, %.0f s, %llu pictures, Indirect capture left out for the previous picture %llu (one end-of-3D copy made %llu); made as before: no previous picture %llu, "
                  "camera cut %llu, A/B exact phase %llu, not the plain copy %llu, no reader %llu, atlas reader %llu, EFB too small %llu, skipped already %llu",
                  IndPrevAb(), secs, static_cast<unsigned long long>(c.pics), static_cast<unsigned long long>(c.used), static_cast<unsigned long long>(c.copies),
                  static_cast<unsigned long long>(c.noPrev), static_cast<unsigned long long>(c.cut), static_cast<unsigned long long>(c.ab), static_cast<unsigned long long>(c.shape),
                  static_cast<unsigned long long>(c.noRead), static_cast<unsigned long long>(c.atlas), static_cast<unsigned long long>(c.small), static_cast<unsigned long long>(c.skipped));
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    g_ipCtr = IpCount{};
}
int PrevCapMask() { return static_cast<int>(VGX_KNOB(static_cast<int32_t>(std::clamp<long>(EnvInt("GXV_PREVCAP", 0), 0, 7)))); }
int PrevCapCensusOn() { return static_cast<int>(VGX_KNOB(EnvInt("GXV_PREVCAP_CENSUS", 0) != 0 ? 1 : 0)); }
bool PcCamCut() {
    const uint32_t cuts = port_camera_cut_count ? port_camera_cut_count() : 0;
    if (cuts != g_pcCutSeen) { g_pcCutSeen = cuts; g_pcCutFrame = g_frame; }
    const long pics = static_cast<long>(VGX_KNOB(static_cast<int32_t>(std::clamp<long>(EnvInt("GXV_PREVCAP_CAMPICS", 2), 0, 600))));
    return g_pcCutFrame != 0 && g_frame < g_pcCutFrame + static_cast<uint64_t>(pics) + 1;
}
// (census) one row per class: copies, destination pixels, draws after the copy in the picture, readers
struct PcClass { uint64_t n = 0, px = 0, after = 0, readers = 0, whole = 0; };
enum { PCC_INDIRECT, PCC_A8WHOLE, PCC_A8TILE, PCC_COLWHOLE, PCC_TILE, PCC_PASS, PCC_DEPTH, PCC_ATLAS, PCC_COUNT };
const char* const kPcName[PCC_COUNT] = {"indirect(RGB565 whole, marked)", "alpha-only whole (shadow silhouette)", "alpha-only tile (shadow corner, clip mask)",
                                        "colour whole (bloom scene, filters)", "colour tile (bloom/blur chain)", "from an offscreen pass", "depth", "from an atlas cell"};
PcClass g_pcCls[PCC_COUNT], g_pcClsTotal[PCC_COUNT];
uint64_t g_pcCensusPics = 0, g_pcCensusDraws = 0, g_pcCensusLast = 0;
void PcCensus() {
    ++g_pcCensusPics;
    const size_t n = g_buf.cmds.size();
    size_t draws = 0;
    for (size_t i = 0; i < n; ++i) draws += g_buf.cmds[i].type == CMD_DRAW;
    g_pcCensusDraws += draws;
    size_t seen = 0;
    for (size_t i = 0; i < n; ++i) {
        if (g_buf.cmds[i].type == CMD_DRAW) { ++seen; continue; }
        if (g_buf.cmds[i].type != CMD_COPY_TEX) continue;
        const CopyTexCmd& c = g_buf.copyTex[g_buf.cmds[i].arg];
        const bool whole = c.src.x == 0 && c.src.y == 0 && c.src.w == g.efbW && c.src.h == g.efbH;
        const bool alphaOnly = c.mat[3][3] == 1.0f && c.mat[0][3] == 1.0f && c.mat[0][0] == 0.0f; // (A8: every channel = alpha)
        int k = PCC_TILE;
        if (c.srcTex || c.dstTexW) k = PCC_ATLAS;
        else if (c.depth) k = PCC_DEPTH;
        else if (c.fromPass) k = PCC_PASS;
        else if (c.transient && !c.dry) k = PCC_INDIRECT;
        else if (alphaOnly) k = whole ? PCC_A8WHOLE : PCC_A8TILE;
        else if (whole) k = PCC_COLWHOLE;
        PcClass& r = g_pcCls[k];
        ++r.n;
        r.px += static_cast<uint64_t>(c.dstW) * c.dstH;
        r.whole += whole ? 1 : 0;
        r.after += draws - seen;
        for (size_t j = i + 1; j < n; ++j) {
            const Cmd& x = g_buf.cmds[j];
            if (x.type == CMD_DRAW) {
                const DrawRec& d = g_buf.draws[x.arg];
                bool rd = false;
                for (int m = 0; m < 8 && !rd; ++m) rd = (d.texMask & (1u << m)) && d.tex[m].texId == c.texId;
                r.readers += rd ? 1 : 0;
            } else if (x.type == CMD_COPY_TEX && g_buf.copyTex[x.arg].texId == c.texId && !g_buf.copyTex[x.arg].skip) {
                break;
            }
        }
    }
    if (g_pcCensusPics - g_pcCensusLast < 600) return;
    const double p = static_cast<double>(g_pcCensusPics - g_pcCensusLast);
    char line[1400];
    int len = std::snprintf(line, sizeof line, "prevcap-census: %.0f pictures, %.1f draws a picture; EFB copies a picture (count / of the whole EFB / kilopixels written / readers / draws after it):",
                            p, static_cast<double>(g_pcCensusDraws) / p);
    for (int k = 0; k < PCC_COUNT && len > 0 && len < static_cast<int>(sizeof line) - 200; ++k) {
        const PcClass& r = g_pcCls[k];
        if (!r.n) continue;
        const double c = static_cast<double>(r.n);
        len += std::snprintf(line + len, sizeof line - static_cast<size_t>(len), " [%s: %.2f / %.2f / %.0fk / %.1f / %.0f]", kPcName[k], c / p, static_cast<double>(r.whole) / p,
                             static_cast<double>(r.px) / c / 1000.0, static_cast<double>(r.readers) / c, static_cast<double>(r.after) / c);
    }
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    for (int k = 0; k < PCC_COUNT; ++k) { g_pcClsTotal[k].n += g_pcCls[k].n; g_pcCls[k] = PcClass{}; }
    g_pcCensusLast = g_pcCensusPics;
    g_pcCensusDraws = 0;
}
void PcLog() {
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::seconds(60) || !g_pcCtr.pictures) return;
    const double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    const PcCount& c = g_pcCtr;
    char line[700];
    std::snprintf(line, sizeof line,
                  "prevcap: GXV_PREVCAP=%d, %.0f s, %llu pictures, %llu marked captures: left out for the previous picture %llu (%llu reader draws pointed at it, %llu of an earlier picture's "
                  "capture), kept for the next picture %llu; made as before: no previous picture %llu, camera cut %llu, a reader reads alpha %llu, an atlas copy reads it %llu, no reader %llu, "
                  "not the plain copy %llu, skipped already %llu, direct display %llu",
                  PrevCapMask(), secs, static_cast<unsigned long long>(c.pictures), static_cast<unsigned long long>(c.marked), static_cast<unsigned long long>(c.used),
                  static_cast<unsigned long long>(c.readers), static_cast<unsigned long long>(c.pre), static_cast<unsigned long long>(c.keeps), static_cast<unsigned long long>(c.noPrev),
                  static_cast<unsigned long long>(c.cut), static_cast<unsigned long long>(c.alpha), static_cast<unsigned long long>(c.atlas), static_cast<unsigned long long>(c.noRead),
                  static_cast<unsigned long long>(c.shape), static_cast<unsigned long long>(c.skipped), static_cast<unsigned long long>(c.direct));
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    g_pcTotal.pictures += c.pictures; g_pcTotal.used += c.used;
    g_pcCtr = PcCount{};
}
} // namespace
void PrevCapCounters(uint64_t out[16]) {
    const PcCount& c = g_pcCtr;
    const PcCount& t = g_pcTotal;
    const uint64_t v[16] = {t.pictures + c.pictures, c.marked, c.used, c.readers, c.pre, c.keeps, c.noPrev, c.cut, c.alpha, c.atlas, c.noRead, c.shape, c.direct, c.skipped, 0, 0};
    for (int i = 0; i < 16; ++i) out[i] = v[i];
}
static void PrevCapPass() {
    if (PrevCapCensusOn()) PcCensus();
    const bool ip = IndPrevOn();
    const int mask = ip ? 1 : PrevCapMask();
    if (!mask) return;
    ++g_pcCtr.pictures;
    if (ip) ++g_ipCtr.pics;
    if (!(mask & 1)) { PcLog(); return; }
    const long abN = ip ? IndPrevAb() : 0;
    const bool abExact = abN > 0 && ((g_ipPic++ / static_cast<uint64_t>(abN)) & 1u) != 0; // (odd blocks of N pictures: the game's own capture)
    static const bool directDisplay = vgx::dd::EffectiveMode(EnvInt("GXV_DIRECT_DISPLAY", 0), EnvInt("GXV_HRW", 0), EnvInt("GXV_DIRECT_DISPLAY_HRW", 0), EnvInt("GXV_DD_DYN", 0)) != 0; // (the EFB's buffer then goes to the display: no previous picture to keep)
    const size_t n = g_buf.cmds.size();
    size_t disp = n;
    for (size_t i = n; i-- > 0;)
        if (g_buf.cmds[i].type == CMD_COPY_DISP) { disp = i; break; }
    const uint32_t stage = g_transientStage;
    const bool camCut = PcCamCut();
    const bool prevOk = !IcHrwPairing() && g_pcKeepFrame != 0 && // (GXV_HRW_CAPFIX: with pairs the previous A is a pair back, not a picture)
         g_pcKeepFrame + 1 == g_frame && g_pcKeepStage == stage && g_pcKeepW == g.efbW && g_pcKeepH == g.efbH && g_pcTexId != 0;
    bool keepWanted = false;
    for (size_t i = 0; i < n; ++i) {
        if (g_buf.cmds[i].type != CMD_COPY_TEX) continue;
        CopyTexCmd& c = g_buf.copyTex[g_buf.cmds[i].arg];
        if (!c.transient || c.dry) continue;
        ++g_pcCtr.marked;
        if (c.skip) { ++g_pcCtr.skipped; if (ip) ++g_ipCtr.skipped; continue; } // (GXV_INDCAP_SKIP / _HALFRATE left it out already: its texture holds an older real capture)
        uint32_t valid = 0; // (the conversion's channels that are the EFB's own)
        for (int r = 0; r < 4; ++r) {
            bool same = c.add[r] == 0.0f;
            for (int col = 0; col < 4 && same; ++col) same = c.mat[r][col] == (r == col ? 1.0f : 0.0f);
            if (same) valid |= 1u << r;
        }
        const bool plain = !c.fromPass && !c.passMore && !c.srcTex && !c.dstTexW && !c.depth && !c.fill && c.texId != 0 && !c.clear.enable && c.src.x == 0 && c.src.y == 0 &&
                           c.src.w == g.efbW && c.src.h == g.efbH && c.dstW == g.efbW && c.dstH == g.efbH && (valid & 7u) == 7u && g.efbW >= 64 && g.efbH >= 64;
        if (!plain) { ++g_pcCtr.shape; if (ip) ++g_ipCtr.shape; continue; }
        if (directDisplay && !ip) { ++g_pcCtr.direct; continue; }
        // the readers: every draw of the picture that samples the texture up to its next write (before the copy: the previous picture's capture)
        uint32_t need = 0;
        size_t nBefore = 0, nAfter = 0;
        bool atlas = false;
        for (size_t j = 0; j < n; ++j) {
            const Cmd& x = g_buf.cmds[j];
            if (x.type == CMD_DRAW) {
                const DrawRec& d = g_buf.draws[x.arg];
                for (int m = 0; m < 8; ++m) {
                    if (!(d.texMask & (1u << m)) || d.tex[m].texId != c.texId) continue;
                    const uint32_t chan = d.fsId < g_fsReg.sources.size() && g_fsReg.sources[d.fsId] ? g_fsReg.sources[d.fsId]->texChan : 0xFFFFFFFFu;
                    need |= (chan >> (4 * m)) & 0xFu;
                    ++(j < i ? nBefore : nAfter);
                }
            } else if (x.type == CMD_COPY_TEX && j != i) {
                const CopyTexCmd& y = g_buf.copyTex[x.arg];
                if (y.srcTex == c.texId) atlas = true;
                else if (y.texId == c.texId && !y.skip && j > i) break; // (written again: what follows reads that copy)
            } else if (j > i && ((x.type == CMD_TEX_FREE && x.arg == c.texId) || (x.type == CMD_TEX_UPLOAD && g_buf.uploads[x.arg].texId == c.texId))) {
                break;
            }
        }
        if (atlas) { ++g_pcCtr.atlas; if (ip) ++g_ipCtr.atlas; continue; }
        if (nBefore + nAfter == 0) { ++g_pcCtr.noRead; if (ip) ++g_ipCtr.noRead; continue; }
        if ((need & 8u) && !ip) { ++g_pcCtr.alpha; continue; } // (INDPREV's copy writes alpha 1, as the game's RGB565 capture reads)
        if (!g_pcTexId) g_pcTexId = AcquireCopyTexture(g_pcKey, static_cast<uint16_t>(g.efbW), static_cast<uint16_t>(g.efbH), 4);
        keepWanted = true; // (this picture's own display copy is what the next picture reads)
        // the readers of this picture are pointed at the keeper when the picture before it asked for one: the capture is left out. A picture
        // that makes its capture for real after one that left it out still points the readers BEFORE the capture at the keeper.
        const bool lastLeft = g_pcLeftTex == c.texId && g_pcLeftFrame + 1 == g_frame;
        if (ip && abExact) { ++g_ipCtr.ab; continue; }
        if (!prevOk) { ++g_pcCtr.noPrev; if (ip) ++g_ipCtr.noPrev; continue; }
        if (camCut) { ++g_pcCtr.cut; if (ip) ++g_ipCtr.cut; if (lastLeft) { /* (the readers before the copy read an older capture: the previous picture is the better one) */ } else continue; }
        const bool leave = !camCut;
        size_t to = n;
        if (leave) {
            for (size_t j = i + 1; j < n; ++j) {
                const Cmd& x = g_buf.cmds[j];
                if ((x.type == CMD_COPY_TEX && g_buf.copyTex[x.arg].texId == c.texId && !g_buf.copyTex[x.arg].skip) ||
                    (x.type == CMD_TEX_FREE && x.arg == c.texId) || (x.type == CMD_TEX_UPLOAD && g_buf.uploads[x.arg].texId == c.texId)) { to = j; break; }
            }
        } else {
            to = i; // (only the readers before the capture)
        }
        for (size_t j = 0; j < to; ++j) {
            if (g_buf.cmds[j].type != CMD_DRAW) continue;
            DrawRec& d = g_buf.draws[g_buf.cmds[j].arg];
            for (int m = 0; m < 8; ++m) {
                if (!(d.texMask & (1u << m)) || d.tex[m].texId != c.texId) continue;
                d.tex[m].texId = g_pcTexId;
                ++(leave ? g_pcCtr.readers : g_pcCtr.pre);
            }
        }
        if (leave) {
            c.skip = true;
            ++g_pcCtr.used;
            if (ip) ++g_ipCtr.used;
            g_pcLeftTex = c.texId;
            g_pcLeftFrame = g_frame;
        }
    }
    if (keepWanted && ip) {
        if (g.efbW < 64 || g.efbH < 64) { ++g_ipCtr.small; }
        else {
            // the keeper: after the last draw that is not the HUD's (before the display copy when there is none), the whole EFB 1:1 as RGB565 looks (alpha 1)
            size_t ins = disp < n ? disp : n;
            for (size_t i = n; i-- > 0;) {
                if (g_buf.cmds[i].type == CMD_DRAW && !g_buf.draws[g_buf.cmds[i].arg].hud) { ins = i + 1; break; }
            }
            if (ins > disp) ins = disp;
            CopyTexCmd k;
            k.texId = g_pcTexId;
            k.src = EfbRect{0, 0, static_cast<int16_t>(g.efbW), static_cast<int16_t>(g.efbH)};
            k.dstW = static_cast<uint16_t>(g.efbW);
            k.dstH = static_cast<uint16_t>(g.efbH);
            for (int r = 0; r < 3; ++r) k.mat[r][r] = 1.0f;
            k.add[3] = 1.0f;
            g_buf.copyTex.push_back(k);
            g_buf.cmds.insert(g_buf.cmds.begin() + static_cast<std::ptrdiff_t>(ins), Cmd{CMD_COPY_TEX, static_cast<uint32_t>(g_buf.copyTex.size() - 1)});
            g_pcKeepFrame = g_frame;
            g_pcKeepStage = stage;
            g_pcKeepW = g.efbW;
            g_pcKeepH = g.efbH;
            ++g_ipCtr.copies;
        }
    } else if (keepWanted && disp < n) {
        g_buf.copyDisp[g_buf.cmds[disp].arg].keepTex = g_pcTexId;
        g_pcKeepFrame = g_frame;
        g_pcKeepStage = stage;
        g_pcKeepW = g.efbW;
        g_pcKeepH = g.efbH;
        ++g_pcCtr.keeps;
    }
    if (ip) IpLog();
    PcLog();
}

// ---- GXV_EFBSWAP2: full-size copies whose samplers do not see the conversion
// GXV_EFBSWAP (the back end) makes a whole-EFB copy without a copy scene by handing the EFB's colour buffer to the texture, but
// only for a copy with no conversion at all. Every picture of a game scene has two whole-EFB copies with one:
//  - the silhouette / shadow-volume alpha capture (SceneFunction::executeDrawSilhouetteAndFillShadow -> MR::fillSilhouetteColor,
//    ScreenAlphaCapture: GX_CTF_A8, every channel = the EFB's alpha), read at once by one screen quad whose TEV uses TEXA only;
//  - the indirect capture (CaptureScreenDirector "Indirect", every picture while no other timing is asked for: GX_TF_RGB565,
//    colour as it is, alpha 1), read by the indirect models and particles (IndDummy) of the same picture, usually as TEXC only,
//    or by nothing at all.
// Each costs two scenes on the console (the EFB stored, a copy scene, the EFB loaded again). When every draw that samples the copy
// in this frame, before the texture is written again, reads only channels whose conversion row is that channel unchanged (and
// adds nothing), the EFB's buffer gives them exactly what the converted copy would: the copy is marked swapOk and the back end
// swaps it (one scene less, and no copy target). Channels are the shader's (ShaderSource::texChan, FsTexChannels); an atlas copy
// out of the texture counts as reading all four. What a LATER frame reads is not known here: the back end converts the buffer
// it handed over into an ordinary copy at the start of the first frame with a draw that needs another channel (RawConvert).
//   GXV_EFBSWAP2=0 off; 1 (default) on. Read once. Needs GXV_EFBSWAP=1.
bool EfbSwap2Enabled() {
    const bool on = VGX_KNOB(EnvInt("GXV_EFBSWAP2", 1)  /* off by default: r32 froze at the AstroGalaxy load with it on (console, 4 Nov 15:30) */ != 0 && EnvInt("GXV_EFBSWAP", 1) != 0) != 0;
    return on;
}
static void MarkSwapCopies() {
    if (!EfbSwap2Enabled() || g_buf.copyTex.empty()) return;
    const size_t n = g_buf.cmds.size();
    for (size_t i = 0; i < n; ++i) {
        if (g_buf.cmds[i].type != CMD_COPY_TEX) continue;
        CopyTexCmd& c = g_buf.copyTex[g_buf.cmds[i].arg];
        c.swapOk = false;
        c.swapPredictedNo = false;
        // (the shape the back end's EfbSwap takes: the whole EFB 1:1 into a texture of its own, from the EFB's colour)
        if (c.skip || c.fromPass || c.passMore || c.depth || c.fill || c.srcTex || c.dstTexW || c.texId == 0) continue;
        if (c.src.x != 0 || c.src.y != 0 || c.dstW != c.src.w || c.dstH != c.src.h || c.src.w < 64 || c.src.h < 64) continue;
        uint32_t valid = 0; // channels the conversion leaves unchanged
        for (int r = 0; r < 4; ++r) {
            bool same = c.add[r] == 0.0f;
            for (int col = 0; col < 4 && same; ++col) same = c.mat[r][col] == (r == col ? 1.0f : 0.0f);
            if (same) valid |= 1u << r;
        }
        if (valid == 0xFu) continue; // (no conversion: GXV_EFBSWAP's own case)
        uint32_t need = 0;
        for (size_t j = i + 1; j < n && (need & ~valid) == 0; ++j) {
            const Cmd& x = g_buf.cmds[j];
            if (x.type == CMD_DRAW) {
                const DrawRec& d = g_buf.draws[x.arg];
                for (int m = 0; m < 8; ++m) {
                    if (!(d.texMask & (1u << m)) || d.tex[m].texId != c.texId) continue;
                    const uint32_t chan = d.fsId < g_fsReg.sources.size() && g_fsReg.sources[d.fsId] ? g_fsReg.sources[d.fsId]->texChan : 0xFFFFFFFFu;
                    need |= (chan >> (4 * m)) & 0xFu;
                }
            } else if (x.type == CMD_COPY_TEX) {
                const CopyTexCmd& y = g_buf.copyTex[x.arg];
                if (y.srcTex == c.texId) need |= 0xFu;
                else if (y.texId == c.texId) break; // (written again: what follows does not see this copy)
            } else if (x.type == CMD_TEX_FREE && x.arg == c.texId) {
                break;
            } else if (x.type == CMD_TEX_UPLOAD && g_buf.uploads[x.arg].texId == c.texId) {
                break;
            }
        }
        c.swapOk = (need & ~valid) == 0;
        // GXV_EFBSWAP2_PREDICT=1 (default; only with GXV_EFBSWAP2): the readers of the texture's PREVIOUS picture earlier in this
        // frame (from the frame's start, or the last write into it, up to this copy) are what the next frame will most likely do
        // with this copy's picture. If they read a channel the conversion changes, a swap now would be undone next frame by
        // RawConvert (a copy scene at the frame's start, plus the renderer CPU of a target and a buffer change): the ordinary copy
        // is made instead. A guess about speed only; either way the picture is exact (RawPrescan still converts any reader).
        static const bool predict = EnvInt("GXV_EFBSWAP2_PREDICT", 1) != 0;
        if (c.swapOk && predict) {
            uint32_t before = 0;
            for (size_t j = i; j-- > 0 && (before & ~valid) == 0;) {
                const Cmd& x = g_buf.cmds[j];
                if (x.type == CMD_DRAW) {
                    const DrawRec& d = g_buf.draws[x.arg];
                    for (int m = 0; m < 8; ++m) {
                        if (!(d.texMask & (1u << m)) || d.tex[m].texId != c.texId) continue;
                        const uint32_t chan = d.fsId < g_fsReg.sources.size() && g_fsReg.sources[d.fsId] ? g_fsReg.sources[d.fsId]->texChan : 0xFFFFFFFFu;
                        before |= (chan >> (4 * m)) & 0xFu;
                    }
                } else if (x.type == CMD_COPY_TEX) {
                    const CopyTexCmd& y = g_buf.copyTex[x.arg];
                    if (y.srcTex == c.texId) before |= 0xFu;
                    else if (y.texId == c.texId && !y.skip) break; // (written earlier in this frame: what comes before it read older pictures)
                } else if ((x.type == CMD_TEX_FREE && x.arg == c.texId) || (x.type == CMD_TEX_UPLOAD && g_buf.uploads[x.arg].texId == c.texId)) {
                    break;
                }
            }
            if (before & ~valid) {
                c.swapOk = false;
                c.swapPredictedNo = true;
            }
        }
    }
}

// ---- GXV_CAPREAD (water23, diagnostic only; default 0): who samples the whole-EFB screen captures, and how
// The Honeyhive 1 start waterfall shows hard-edged blocks on the console (water23 REPORT.txt). Its draws sample the game's
// "Indirect" screen capture (CaptureScreenDirector, whole EFB, RGB565) or the texture of an earlier picture's capture. This
// names them from the console's own command stream: for one picture every GXV_CAPREAD_EVERY pictures (default 300) that has a
// whole-EFB copy with readers, one "capread:" basic-log line per copy (texture, conversion, swap / skip / fill state) and per
// reader draw (up to GXV_CAPREAD_MAX, default 24): before or after the copy in the picture, J3D list or streamed vertices,
// vertex count, shader ids, every texgen (type, source, projective, post matrix, per-vertex matrix), the indirect stages, the
// sampled texmaps, blend / depth state, scissor and viewport (EFB pixels) and the known pixel extent. Reads the finished frame
// only: nothing drawn, kept or left out changes (the host test compares pictures with it on and off).
//   GXV_CAPREAD=0 off (default); 1 log as above.   GXV_CAPREAD_EVERY=<pictures>, GXV_CAPREAD_MAX=<lines>.
namespace {
uint64_t g_crCtr[7] = {}; // pictures looked at, whole-EFB copies, readers before their copy, after it, logged pictures, reader lines,
                          // draws left out by GXV_CAPREAD_HIDE
int CapReadMode() { return static_cast<int>(VGX_KNOB(static_cast<int32_t>(std::clamp<long>(EnvInt("GXV_CAPREAD", 0), 0, 1)))); }
void CapReadLine(const char* line) {
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
}
} // namespace
void CapReadCounters(uint64_t out[7]) {
    for (int i = 0; i < 7; ++i) out[i] = g_crCtr[i];
}
static void CapReadLog() {
    if (!CapReadMode() || g_buf.copyTex.empty()) return;
    // GXV_CAPREAD_HIDE (water23 round 2; A/B ONLY, changes the picture; read with GXV_CAPREAD=1 only): 1 leaves out every draw that
    // samples a whole-EFB capture texture before this picture's copy into it (it reads the previous picture's capture), 2 every draw
    // after the copy. Tells on the console which of the two kinds draws the waterfall's hard-edged blocks.
    static const long hide = std::clamp<long>(EnvInt("GXV_CAPREAD_HIDE", 0), 0, 2);
    // Round 3 A/B switches (water23; A/B ONLY, they change the picture; with GXV_CAPREAD=1 only). For every draw that reads a whole-EFB
    // capture after it is made (the Indirect models and particles):
    //   GXV_CAPREAD_INDFILT=1 its indirect maps sampled with linear filtering (min and mag), =2 with nearest;
    //   GXV_CAPREAD_NOWARP=1  its indirect matrices zeroed in its TEV block (no warp: the capture as it is at each pixel). The block is
    //                         the arena's deduplicated copy: other draws with the very same TEV values lose their warp too.
    static const long indFilt = std::clamp<long>(EnvInt("GXV_CAPREAD_INDFILT", 0), 0, 2);
    static const long noWarp = std::clamp<long>(EnvInt("GXV_CAPREAD_NOWARP", 0), 0, 1);
    static const long every = std::max(1L, EnvInt("GXV_CAPREAD_EVERY", 300));
    static const long maxLines = std::clamp<long>(EnvInt("GXV_CAPREAD_MAX", 24), 1, 400);
    static uint64_t lastLogged = 0;
    ++g_crCtr[0];
    const size_t n = g_buf.cmds.size();
    const bool due = lastLogged == 0 || g_frame >= lastLogged + static_cast<uint64_t>(every);
    long lines = 0;
    bool logged = false;
    char line[900];
    for (size_t i = 0; i < n; ++i) {
        if (g_buf.cmds[i].type != CMD_COPY_TEX) continue;
        const CopyTexCmd& c = g_buf.copyTex[g_buf.cmds[i].arg];
        if (c.fromPass || c.depth || c.srcTex || c.dstTexW || c.texId == 0) continue;
        if (c.src.x != 0 || c.src.y != 0 || c.src.w != g.efbW || c.src.h != g.efbH) continue; // (whole-EFB captures only)
        ++g_crCtr[1];
        // the readers: draws sampling the texture from the previous write of it (or the picture's start) to the next one
        size_t from = 0, to = n;
        for (size_t j = i; j-- > 0;)
            if (g_buf.cmds[j].type == CMD_COPY_TEX && g_buf.copyTex[g_buf.cmds[j].arg].texId == c.texId) { from = j + 1; break; }
        for (size_t j = i + 1; j < n; ++j)
            if (g_buf.cmds[j].type == CMD_COPY_TEX && g_buf.copyTex[g_buf.cmds[j].arg].texId == c.texId) { to = j; break; }
        uint32_t before = 0, after = 0;
        for (size_t j = from; j < to; ++j) {
            if (g_buf.cmds[j].type != CMD_DRAW) continue;
            DrawRec& d = g_buf.draws[g_buf.cmds[j].arg];
            bool reads = false;
            for (int m = 0; m < 8 && !reads; ++m) reads = (d.texMask & (1u << m)) && d.tex[m].texId == c.texId;
            if (!reads) continue;
            ++(j < i ? before : after);
            // (GXV_CAPREAD_HIDE, A/B only: the draws that read an earlier picture's capture (=1), or this picture's (=2), are left out)
            if ((hide == 1 && j < i) || (hide == 2 && j > i)) { d.idxCount = 0; ++g_crCtr[6]; }
            if (j > i && (indFilt || noWarp) && d.fsId < g_fsReg.keys.size()) {
                const FsKey& fk = g_fsReg.keys[d.fsId];
                for (int k = 0; k < fk.numIndStages && k < 4 && indFilt; ++k) {
                    TexBind& tb = d.tex[fk.ind[k].texMap & 7];
                    tb.minFilter = tb.magFilter = indFilt == 1 ? FILT_LINEAR : FILT_NEAR;
                }
                if (noWarp && fk.numIndStages && d.ublock[UB_TEV] != kNoBlock && static_cast<size_t>(d.ublock[UB_TEV]) + TEV_INDMTX + 24 <= g_buf.uni.size()) {
                    float* im = g_buf.uni.data() + d.ublock[UB_TEV] + TEV_INDMTX;
                    for (int r = 0; r < 6; ++r)
                        for (int col = 0; col < 3; ++col) im[r * 4 + col] = 0.0f;
                }
            }
        }
        g_crCtr[2] += before;
        g_crCtr[3] += after;
        if (!due || (before + after) == 0) continue;
        if (!logged) { logged = true; ++g_crCtr[4]; lastLogged = g_frame; }
        uint32_t valid = 0;
        for (int r = 0; r < 4; ++r) {
            bool same = c.add[r] == 0.0f;
            for (int col = 0; col < 4 && same; ++col) same = c.mat[r][col] == (r == col ? 1.0f : 0.0f);
            if (same) valid |= 1u << r;
        }
        std::snprintf(line, sizeof line,
                      "capread: picture %llu cmd %u/%u: whole-EFB copy %ux%u into texture %u (unchanged channels 0x%x; marked %d swapOk %d skip %d fill %d "
                      "clear %d): %u readers before it (an earlier picture's capture), %u after it",
                      static_cast<unsigned long long>(g_frame), static_cast<unsigned>(i), static_cast<unsigned>(n), c.dstW, c.dstH, c.texId, valid,
                      c.transient ? 1 : 0, c.swapOk ? 1 : 0, c.skip ? 1 : 0, c.fill ? 1 : 0, c.clear.enable ? 1 : 0, before, after);
        CapReadLine(line);
        for (size_t j = from; j < to && lines < maxLines; ++j) {
            if (g_buf.cmds[j].type != CMD_DRAW) continue;
            const DrawRec& d = g_buf.draws[g_buf.cmds[j].arg];
            int slot = -1;
            for (int m = 0; m < 8 && slot < 0; ++m)
                if ((d.texMask & (1u << m)) && d.tex[m].texId == c.texId) slot = m;
            if (slot < 0) continue;
            ++lines;
            ++g_crCtr[5];
            char tg[200] = "", ind[160] = "", maps[64] = "";
            size_t o = 0;
            if (d.vsId < g_vsReg.keys.size()) {
                const VsKey& vk = g_vsReg.keys[d.vsId];
                for (int t = 0; t < vk.numTexGens && t < 8 && o + 24 < sizeof tg; ++t)
                    o += static_cast<size_t>(std::snprintf(tg + o, sizeof tg - o, "%s%d:t%u s%u%s%s%s", t ? " " : "", t, vk.tg[t].type, vk.tg[t].src,
                                                           vk.tg[t].proj ? " proj" : "", vk.tg[t].post ? (vk.tg[t].post == 2 ? " postN" : " post") : "",
                                                           vk.tg[t].perVtxMtx ? " pvm" : ""));
            }
            if (d.fsId < g_fsReg.keys.size()) {
                const FsKey& fk = g_fsReg.keys[d.fsId];
                o = 0;
                for (int k = 0; k < fk.numIndStages && k < 4 && o + 24 < sizeof ind; ++k)
                    o += static_cast<size_t>(std::snprintf(ind + o, sizeof ind - o, "%sI%d map%u tc%u", k ? " " : "", k, fk.ind[k].texMap, fk.ind[k].texCoord));
                for (int s = 0; s < fk.numStages && s < 16 && o + 32 < sizeof ind; ++s)
                    if (fk.stage[s].ind) o += static_cast<size_t>(std::snprintf(ind + o, sizeof ind - o, " st%d=%06x", s, fk.stage[s].ind & 0x1FFFFFu));
                o = 0;
                for (int s = 0; s < fk.numStages && s < 16 && o + 12 < sizeof maps; ++s)
                    if (fk.stage[s].texEnable)
                        o += static_cast<size_t>(std::snprintf(maps + o, sizeof maps - o, "%s%u/%u", o ? "," : "", fk.stage[s].texMap, fk.stage[s].texCoord));
            }
            const RenderState& rs = d.rs;
            // (round 2: the model-view rows the draw is placed with, as a hash and its translation column: two draws of one model
            // instance have the same; the depth compare; whether GXV_CAPREAD_HIDE left it out)
            uint32_t mvHash = 2166136261u;
            float mvT[3] = {0.0f, 0.0f, 0.0f};
            if (d.ublock[UB_MV] != kNoBlock && static_cast<size_t>(d.ublock[UB_MV]) + 12 <= g_buf.uni.size()) {
                const float* mv = g_buf.uni.data() + d.ublock[UB_MV];
                uint32_t w[12];
                std::memcpy(w, mv, sizeof w);
                for (uint32_t x : w) mvHash = (mvHash ^ x) * 16777619u;
                mvT[0] = mv[3]; mvT[1] = mv[7]; mvT[2] = mv[11];
            }
            std::snprintf(line, sizeof line,
                          "capread:   %s draw cmd %u: %s, %u vertices, vs %u fs %u, slot %d (wrap %u/%u filt %u/%u); texgens [%s]; ind [%s]; stage maps/coords "
                          "[%s]; blend %d %u/%u z %d/%d func %u; mv %08x t %.1f,%.1f,%.1f; scissor %d,%d %dx%d viewport %.0f,%.0f %.0fx%.0f; extent %d,%d %dx%d%s",
                          j < i ? "BEFORE" : "after", static_cast<unsigned>(j), d.vbuf ? "J3D list (static buffer)" : "streamed vertices", d.vtxCount,
                          d.vsId, d.fsId, slot, d.tex[slot].wrapS, d.tex[slot].wrapT, d.tex[slot].minFilter, d.tex[slot].magFilter, tg, ind, maps,
                          rs.blend, rs.blendSrc, rs.blendDst, rs.depthTest, rs.depthWrite, rs.depthFunc, mvHash, mvT[0], mvT[1], mvT[2], rs.scX, rs.scY,
                          rs.scW, rs.scH, rs.vpX, rs.vpY, rs.vpW, rs.vpH, d.exX, d.exY, d.exW, d.exH, d.idxCount == 0 ? " (left out: GXV_CAPREAD_HIDE)" : "");
            CapReadLine(line);
            // (round 3: every texmap the draw samples with its filter / wrap and, for an indirect key, its size; the indirect matrices as
            // the TEV block holds them (texel units: entries times 2^scale); the first two texgens' matrix rows (u_tm); the vertex key's
            // per-vertex position matrix kind (0 none, 1 full palette, 2/3 short / small))
            {
                char extra[700];
                size_t eo = 0;
                const float* tev = d.ublock[UB_TEV] != kNoBlock && static_cast<size_t>(d.ublock[UB_TEV]) + TEV_TEXSIZE + 16 <= g_buf.uni.size()
                                       ? g_buf.uni.data() + d.ublock[UB_TEV] : nullptr;
                for (int m = 0; m < 8 && eo + 64 < sizeof extra; ++m) {
                    if (!(d.texMask & (1u << m))) continue;
                    const TexBind& tb = d.tex[m];
                    eo += static_cast<size_t>(std::snprintf(extra + eo, sizeof extra - eo, "%smap%d tex %u filt %u/%u wrap %u/%u", eo ? "; " : "", m, tb.texId,
                                                            tb.minFilter, tb.magFilter, tb.wrapS, tb.wrapT));
                    if (tev && d.fsId < g_fsReg.keys.size() && g_fsReg.keys[d.fsId].numIndStages)
                        eo += static_cast<size_t>(std::snprintf(extra + eo, sizeof extra - eo, " %gx%g", tev[TEV_TEXSIZE + m * 2], tev[TEV_TEXSIZE + m * 2 + 1]));
                }
                if (tev && d.fsId < g_fsReg.keys.size() && g_fsReg.keys[d.fsId].numIndStages && eo + 120 < sizeof extra) {
                    const float* im = tev + TEV_INDMTX;
                    eo += static_cast<size_t>(std::snprintf(extra + eo, sizeof extra - eo, "; indmtx0 (%g %g %g / %g %g %g) scale %g; indscale %g %g", im[0], im[1], im[2],
                                                            im[4], im[5], im[6], im[3], tev[TEV_INDSCALE], tev[TEV_INDSCALE + 1]));
                }
                const int ntg = d.vsId < g_vsReg.keys.size() ? g_vsReg.keys[d.vsId].numTexGens : 0;
                if (ntg >= 2 && d.ublock[UB_TEXM] != kNoBlock && static_cast<size_t>(d.ublock[UB_TEXM]) + 24 <= g_buf.uni.size() && eo + 200 < sizeof extra) {
                    const float* tm = g_buf.uni.data() + d.ublock[UB_TEXM];
                    eo += static_cast<size_t>(std::snprintf(extra + eo, sizeof extra - eo,
                                                            "; tm0 (%.4g %.4g %.4g %.4g / %.4g %.4g %.4g %.4g / %.4g %.4g %.4g %.4g) tm1 (%.4g %.4g %.4g %.4g / %.4g %.4g %.4g %.4g)",
                                                            tm[0], tm[1], tm[2], tm[3], tm[4], tm[5], tm[6], tm[7], tm[8], tm[9], tm[10], tm[11], tm[12], tm[13],
                                                            tm[14], tm[15], tm[16], tm[17], tm[18], tm[19]));
                }
                if (d.vsId < g_vsReg.keys.size() && eo + 40 < sizeof extra)
                    eo += static_cast<size_t>(std::snprintf(extra + eo, sizeof extra - eo, "; perVtxPos %u; prim %u, %u indices", g_vsReg.keys[d.vsId].perVtxPos,
                                                            d.prim, d.idxCount));
                std::snprintf(line, sizeof line, "capread:     cmd %u detail: %s", static_cast<unsigned>(j), extra);
                CapReadLine(line);
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
// GXV_SHADOWFILL_BOUNDS (ShadowFillPass, before this pass): per copy of the frame, 1 when the copy reads only the EFB's alpha and that
// alpha is known to be 0 over its whole source (what pass isolation's history cannot know: draws that write the constant 0).
std::vector<uint8_t> g_sfZeroCopy;
void SfNoteFill(bool kept);

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
    return static_cast<uint8_t>((rs.colorWrite ? CH_C : 0) | ((rs.alphaWrite || rs.dstAlphaZero) ? CH_A : 0) | ((rs.depthTest && rs.depthWrite) ? CH_Z : 0));
}
inline uint8_t DrawReads(const RenderState& rs) {
    uint8_t m = 0;
    if (rs.depthTest && rs.depthFunc != CMP_ALWAYS) m |= CH_Z;
    if (rs.blend) {
        // The colour (and, when it is written, the alpha) underneath goes into the result; the
        // alpha plane also when it is one of the blend factors.
        const bool dstAlphaFactor = rs.blendSrc == BF_DST_ALPHA || rs.blendSrc == BF_INV_DST_ALPHA || rs.blendDst == BF_DST_ALPHA || rs.blendDst == BF_INV_DST_ALPHA;
        if (rs.colorWrite) m |= CH_C;
        if (rs.alphaWrite || rs.dstAlphaZero || dstAlphaFactor) m |= CH_A;
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
    const bool on = VGX_KNOB(EnvInt("GXV_NOOPCLEAR", 1) != 0) != 0;
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
    const bool on = VGX_KNOB(EnvInt("GXV_PASSISO", 1) != 0) != 0;
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
                bool known0 = !t.skip && !t.depth && HistKnown(copyReads, t.src, v);
                const bool sfZero = !known0 && !t.skip && !t.depth && copyReads == CH_A && c.arg < g_sfZeroCopy.size() && g_sfZeroCopy[c.arg];
                if (sfZero) { // (GXV_SHADOWFILL_BOUNDS: the alpha it reads is 0 everywhere; it reads nothing else)
                    v[0] = v[1] = v[2] = v[3] = 0.0f;
                    v[4] = 1.0f;
                    known0 = true;
                }
                if (known0) {
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
                    if (sfZero) SfNoteFill(t.skip);
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

// ================================================================ shadow fill bounds (GXV_SHADOWFILL_BOUNDS), part 2: the frame pass
// SceneFunction::executeDrawSilhouetteAndFillShadow -> MR::fillSilhouetteColor, every game picture: ScreenAlphaCapture copies the
// whole EFB's alpha into a texture (GX_CTF_A8: every channel = the EFB's alpha), and a screen quad samples it with one TEV stage
// (alpha = TEXA > konst ? A0 : 0) and the alpha test GREATER 0, blended 50 % black: where the texture is 0 the quad is discarded.
// The EFB's alpha is 0 almost everywhere: the display copy clears it to 0 (MainLoopFramework, (0,0,0,0)), the frame's clear writes
// GXSetDstAlpha(TRUE, 0), the opaque scene writes the constant 0 (GameScene::draw3D: GXSetDstAlpha(TRUE, 0)), and only the shadow
// volumes (ShadowVolumeDrawer: ONE / ONE and SUBTRACT into alpha) and the player's silhouette (DrawType 0x28) put anything else
// there. This pass follows, over the finished frame and from frame to frame, a set of EFB rectangles outside which the alpha is
// known to be 0 (N):
//  (a) a draw that writes alpha as the constant 0 (RenderState::dstAlphaZero 1 under blending; or not blended with the fragment
//      shader writing the destination-alpha constant, FsKey::dstAlpha, which its own TEV block holds as 0) or does not write alpha
//      leaves N as it is; one that also covers its extent whole (DrawRec::cover) takes the extent out of N; any other draw that
//      writes alpha adds the pixels it can write (DrawRec::ab, part 1; the extent when those are not known);
//  (b) a clear (of a copy, or of the display copy) takes its rectangle out of N when it clears alpha to 0, and adds it otherwise;
//  (c) N carries over from one frame to the next (the display copy's clear empties it); a change of target makes it everything.
// At the capture (a colour copy that reads only the EFB's alpha, converting it with nothing added: every texel is 0 where the
// alpha is 0), N cut to the copy's source gives the texels that may not be 0. At the next draw that samples that texture:
//  (d) when the draw cannot change a pixel wherever the texels it samples are all 0 - one TEV stage whose alpha is an A8 compare
//      "TEXA > b" (b a non-negative input) added to ZERO, and an alpha test that fails for alpha 0 with the references of its own
//      TEV block - then:
//       - nothing may be other than 0: the draw is left out (it would discard every fragment), and the copy is marked for pass
//         isolation (IsolatePasses) as a copy of a known constant: a fill of 0, and none at all while the texture still holds that
//         (g_constTex), so the copy's EFB store, copy scene and EFB reload go too;
//       - else, when the draw's screen-to-texture mapping is affine (its vertices through its own blocks: equal clip w, texture
//         coordinates from an attribute through a matrix, no projection) and the texture clamps: its scissor is cut to the
//         pixels whose samples can reach a texel within kSfReach-derived EFB pixels of N (the copy's own filtering, the sample's
//         bilinear footprint at the smallest render scale, the scissor's rounding to render pixels), where alone it can change
//         anything (only when that is at most kSfMaxArea of its pixels: a scissor change costs vitaGL mask passes);
//       - else nothing changes.
// The picture is the same, bit for bit: a draw is cut only where every fragment it would draw is discarded, and a copy is only
// made a fill of the value every texel of the real copy has. Anything not proven leaves the frame as it was (the full path).
//   GXV_SHADOWFILL_BOUNDS=1 (default) on; 0 off (no bounds are worked out either). Read once. Basic log about once a minute:
//   "shadowfill: ..." (pictures with the fill left out / scissored / full, and the scissored area).
namespace {
struct SfSet {
    static constexpr int kMax = 16;
    EfbRect r[kMax];
    int n = 0;
    void All() { n = 1; r[0] = EfbRect{-8192, -8192, 16383, 16383}; }
    void Add(const EfbRect& a) {
        if (a.w <= 0 || a.h <= 0) return;
        for (int i = 0; i < n; ++i)
            if (Inside(a, r[i])) return;
        int k = 0;
        for (int i = 0; i < n; ++i)
            if (!Inside(r[i], a)) r[k++] = r[i];
        n = k;
        if (n < kMax) { r[n++] = a; return; }
        // (full: into the rectangle whose union with it grows least; only ever more pixels)
        int best = 0;
        int64_t grow = INT64_MAX;
        for (int i = 0; i < n; ++i) {
            const int64_t gi = RArea(Union(r[i], a)) - RArea(r[i]);
            if (gi < grow) { grow = gi; best = i; }
        }
        r[best] = Union(r[best], a);
    }
    void Sub(const EfbRect& c) {
        if (c.w <= 0 || c.h <= 0) return;
        EfbRect keep[kMax * 4];
        int m = 0;
        for (int i = 0; i < n; ++i) {
            if (!Overlap(r[i], c)) { keep[m++] = r[i]; continue; }
            if (Inside(r[i], c)) continue;
            EfbRect piece[4];
            const int k = RSubtract(r[i], c, piece);
            for (int q = 0; q < k; ++q) keep[m++] = piece[q];
        }
        n = 0;
        for (int i = 0; i < m; ++i) Add(keep[i]);
    }
    // The bounding box of N inside `src`; false: N has no pixel there.
    bool Bound(const EfbRect& src, EfbRect& b) const {
        int x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
        for (int i = 0; i < n; ++i) {
            const int ax = std::max<int>(r[i].x, src.x), ay = std::max<int>(r[i].y, src.y);
            const int bx = std::min<int>(r[i].x + r[i].w, src.x + src.w), by = std::min<int>(r[i].y + r[i].h, src.y + src.h);
            if (bx <= ax || by <= ay) continue;
            x0 = std::min(x0, ax); y0 = std::min(y0, ay); x1 = std::max(x1, bx); y1 = std::max(y1, by);
        }
        if (x1 <= x0 || y1 <= y0) return false;
        b = EfbRect{static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(x1 - x0), static_cast<int16_t>(y1 - y0)};
        return true;
    }
} g_sfN;
bool g_sfInit = false;
// ---- GXV_CORNERPASS (imp45): the front end's half. Mario's shadow corner (CollisionShadow::drawAndCaptureTex: his simple model drawn into the EFB's top-right 160x160
// corner, multiplied by a disc, copied as A8) cuts the EFB's scene every picture. GXV_HOIST_GRID takes the cut out only when the silhouette's whole-EFB alpha capture comes
// just before it (its "first copy"); with GXV_SHADOW_STENCIL on (the Performance default) that capture is left out, so the corner copy cuts the EFB again (night39, Beach
// Bowl: scenes25 "corner: c 1.00 ... s 1.00", scenes23 "GXV_HOIST_GRID=1: planned 0.00"). What the copy reads is known without a first copy when the alpha over its source
// is 0 before the corner's own draws: that is what N says. Here, while ShadowFillPass walks the picture, the state of N over the corner rectangle (the copy's source and
// the 3 EFB pixels of margin the back end's grid pass draws, vgx_backend_gl.cpp HoistGridRect) is followed:
//   ZERO     N has no pixel there (alpha is exactly 0 over all of it);
//   WRITING  it was ZERO, and since then only draws wrote alpha there; `first` is the first of them (kind 2: anything but the constant 0);
//   UNKNOWN  anything else (a clear to another alpha, a target change, a pass, or alpha that was there before the picture).
// A copy that reads only alpha from exactly that source while WRITING is marked (CopyTexCmd::cornerPass, cornerFrom): the alpha it reads is 0 with the alpha-writing
// draws from `first` on, in order, drawn over it. The rectangle is learned from the copy itself, so the first picture (and one after a change of it) is not marked.
// Exactness rests on N (the same bookkeeping GXV_SHADOWFILL_BOUNDS' cuts rest on) and on alpha results depending on alphas only (GX blend factors and logic operations
// take the alpha channel's own values; the replayed draws are refused by the back end if they test depth or carry a stencil operation).
// (CornerPassMode and CornerPassFeCounters are defined after this unnamed namespace, below ShadowFillPass: the back end links to them)
struct CornerTrack {
    enum : uint8_t { UNKNOWN, ZERO, WRITING };
    EfbRect rm{};       // the corner and its margin, clipped to the EFB
    EfbRect src{};      // the copy's source it was learned from
    bool have = false;
    uint8_t state = UNKNOWN;
    uint32_t first = 0; // draw index + 1
    uint64_t seen = 0, marked = 0, whyRect = 0, whyAlpha = 0, whyNoWriter = 0, whyTarget = 0;
    uint64_t byFillMarked = 0, fillsCertified = 0; // (fix47) copies marked on a fill's certificate; certified fills met
    bool targetChanged = false;
    bool byFill = false; // (fix47) the ZERO came from a stencil window's fill, not from N
    void ZeroByFill() { state = ZERO; first = 0; byFill = true; }
    void Recheck() { // after N lost pixels (a constant-0 writer that covers, a clear to alpha 0): ZERO if nothing is left there
        if (!have) return;
        EfbRect b{};
        if (!g_sfN.Bound(rm, b)) { state = ZERO; first = 0; }
    }
    void Begin() {
        targetChanged = false;
        byFill = false;
        state = UNKNOWN;
        first = 0;
        Recheck();
    }
    void Writer(const EfbRect& a, uint32_t drawIdx) { // a kind-2 writer whose box N took
        if (!have || !Overlap(a, rm)) return;
        if (state == ZERO) { state = WRITING; first = drawIdx + 1; }
    }
    void Unknown(bool target) { state = UNKNOWN; first = 0; byFill = false; if (target) targetChanged = true; }
} g_cornerTrack;
// (fix47) The fill's certificate. On the console (Vita3K c36b, Beach Bowl) every copy was marked but none planned: "a replayed draw tests depth 600". The draws that
// test depth are not Mario's (his model is drawn with J3DModelX display list 1 / 14: no depth test, GXSetDstAlpha constants) but the picture's shadow volumes: their
// boxes lie over the top-right corner (or are not known), so N holds the corner from the first volume on and `first` was a volume. What N cannot know is that the
// silhouette fill takes that alpha away again. GXV_SHADOW_STENCIL's pass proves it for a window it takes (ShadowStencilPass): the window starts at the EFB's clear
// (alpha 0 everywhere), every alpha writer in it is a classified layer (a volume's +a / -a with no clamp, a mark's constant), and the checks it makes (each constant
// >= the fill's threshold T, the bound for volumes after a mark) are exactly "at the capture every pixel's alpha is 0 or >= T" - the statement its own exactness rests
// on (stencil != 0 <=> the fill writes). The fill (one quad over the whole EFB, reading its own pixel's captured alpha, GXSetDstAlpha(TRUE, 0)) writes alpha 0 where
// the captured alpha is >= T and nothing elsewhere: after it the alpha is 0 on every pixel of its scissor. That holds however the fill is carried out (through the
// stencil, or from the capture when the window is undone or falls back), except when the back end leaves the fill out (its "fill left out" fallback): the back
// end refuses the corner pass then (CopyTexCmd::cornerFill). Draw indices of this picture's certified fills, set by ShadowStencilPass:
std::vector<uint32_t> g_ssZeroFill;
inline bool SsZeroFill(uint32_t draw) {
    for (uint32_t f : g_ssZeroFill)
        if (f == draw) return true;
    return false;
}
inline EfbRect CornerMargin(const EfbRect& s) { // (vgx_backend_gl.cpp HoistGridRect: the same 3 EFB pixels, the same clip)
    constexpr int kMargin = 3;
    const int x0 = std::max(0, s.x - kMargin), y0 = std::max(0, s.y - kMargin);
    const int x1 = std::min<int>(g.efbW, s.x + s.w + kMargin), y1 = std::min<int>(g.efbH, s.y + s.h + kMargin);
    return EfbRect{static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(x1 - x0), static_cast<int16_t>(y1 - y0)};
}
// The capture copies of this frame whose texture no draw has read yet changed since (texture -> what may not be 0 in it).
struct SfCap {
    uint32_t texId;
    bool zero;   // no texel may be other than 0
    EfbRect b;   // else: N cut to the source, EFB pixels
    EfbRect src;
    uint16_t dstW, dstH;
};
std::vector<SfCap> g_sfCaps;
// The log's window (about a minute): pictures, fills left out, scissored (and their area, screens), left whole, captures made fills
// (and of those, fills not needed: the texture held 0 already).
struct SfCount { uint64_t pictures = 0, skipped = 0, scissored = 0, full = 0, fills = 0, fillsKept = 0; double area = 0; uint64_t shrunk = 0, shrinkFail = 0; double shrinkArea = 0; } g_sfCtr, g_sfTotal;
// GXV_SHADOWFILL_SHRINK (gpugen26, vgx_fillshrink.h): 0 (default) off; 1 the silhouette fill's four corners are moved to the pixels GXV_SHADOWFILL_BOUNDS works out (no scissor change, so no
// vitaGL scissor-mask passes), the scissor cut as before when that cannot be done; 2 the corner move only (a fill it cannot move is drawn whole). Needs GXV_SHADOWFILL_BOUNDS=1; the
// same pixels either way (the fragments inside the window are the very ones the whole quad had). Stage-settable. "shadowfill:" line: shrunk N (mean P% of the screen), M refused.
int ShadowFillShrinkMode() { return static_cast<int>(PORT_STAGE_KNOB(int32_t, std::clamp<long>(port::stageknob::Int("GXV_SHADOWFILL_SHRINK", 0), 0, 2))); }
bool SfShrinkFill(DrawRec& d, int coord, const EfbRect& win);

void SfNoteFill(bool kept) {
    ++g_sfCtr.fills;
    if (kept) ++g_sfCtr.fillsKept;
}
// How the record writes alpha: 0 not at all, 1 the constant 0 only, 2 anything else (see (a) above).
int SfAlphaKind(const DrawRec& d) {
    const RenderState& rs = d.rs;
    if (!rs.alphaWrite) return 0; // (the back end's colour mask: SetColorMask(colorWrite, alphaWrite))
    if (rs.blend && rs.dstAlphaZero == 1) return 1; // (alpha factors ZERO / ZERO: SetBlend's mode 1)
    if (!rs.blend && rs.dstAlphaZero == 0 && d.fsId < g_fsReg.keys.size() && g_fsReg.keys[d.fsId].dstAlpha) {
        const uint32_t tb = d.ublock[UB_TEV];
        if (tb != kNoBlock && static_cast<size_t>(tb) + TEV_MISC + 3 <= g_buf.uni.size() && g_buf.uni[tb + TEV_MISC + 2] == 0.0f) return 1;
    }
    return 2;
}
// GX alpha test of the alpha 0 with references r0, r1 (FsKey::alphaTest; the generator's CompareExpr with ai = 0, and its trimmed
// form, give the same for whole references).
bool SfCmpAt0(uint32_t c, float ref) {
    switch (c) {
    case 0: return false;
    case 1: return 0.0f < ref - 0.5f;
    case 2: return std::fabs(ref) < 0.5f;
    case 3: return 0.0f < ref + 0.5f;
    case 4: return 0.0f > ref + 0.5f;
    case 5: return std::fabs(ref) > 0.5f;
    case 6: return 0.0f > ref - 0.5f;
    default: return true;
    }
}
bool SfAlphaTestPassesAt0(uint8_t t, float r0, float r1) {
    const bool a = SfCmpAt0(t & 7u, r0), b = SfCmpAt0((t >> 3) & 7u, r1);
    switch ((t >> 6) & 3u) {
    case 0: return a && b;
    case 1: return a || b;
    case 2: return a != b;
    default: return a == b;
    }
}
// (d): the draw changes no pixel wherever every texel of its one texture is 0. Its texmap and texture coordinate.
bool SfFillShape(const DrawRec& d, int& map, int& coord) {
    if (d.fsId >= g_fsReg.keys.size()) return false;
    const FsKey& k = g_fsReg.keys[d.fsId];
    if (k.numStages != 1 || k.numIndStages != 0 || !k.alphaTestOn) return false;
    const FsKey::Stage& st = k.stage[0];
    if (!st.texEnable || st.texOff || !(d.texMask & (1u << st.texMap))) return false;
    // (alpha: d + ((a > b) ? c : 0), compare mode (bias 3), "greater" (op 0), A8 (scale 3), a = TEXA, d = ZERO, b TEXA / KONST / ZERO)
    const uint32_t a = st.alpha;
    const uint32_t sd = bits(a, 4, 3), sb = bits(a, 10, 3), sa = bits(a, 13, 3);
    if (bits(a, 16, 2) != 3 || bits(a, 18, 1) != 0 || bits(a, 20, 2) != 3 || sa != 4 || sd != 7) return false;
    if (sb != 4 && sb != 6 && sb != 7) return false;
    const uint32_t tb = d.ublock[UB_TEV];
    if (tb == kNoBlock || static_cast<size_t>(tb) + TEV_MISC + 2 > g_buf.uni.size()) return false;
    if (SfAlphaTestPassesAt0(k.alphaTest, g_buf.uni[tb + TEV_MISC], g_buf.uni[tb + TEV_MISC + 1])) return false;
    map = st.texMap;
    coord = st.texCoord;
    return true;
}
// The pixels (EFB, before the draw's scissor) whose texture coordinate can lie in [s0, s1] x [t0, t1] (infinite: no limit), from the
// draw's own vertices through its blocks; false when the mapping is not known to be affine.
bool SfFillWindow(const DrawRec& d, int coord, double s0, double s1, double t0, double t1, bool& none, EfbRect& out) {
    none = false;
    if (d.vbuf || d.prim != PRIM_TRIANGLES || d.vtxCount < 3 || d.vtxCount > 64 || d.idxCount < 3) return false;
    if (d.vsId >= g_vsReg.keys.size() || d.fsId >= g_fsReg.keys.size()) return false;
    const VsKey& vs = g_vsReg.keys[d.vsId];
    const FsKey& fs = g_fsReg.keys[d.fsId];
    if (vs.perVtxPos || vs.lpMode() || coord >= vs.numTexGens || (fs.texProjMask & (1u << coord))) return false;
    const VsKey::TexGen& tg = vs.tg[coord];
    if (tg.type != 0 || tg.src < 5 || tg.src > 12 || tg.post || tg.perVtxMtx || tg.proj) return false;
    const int k = tg.src - 5;
    if (!(d.layoutMask & (VL_TEX0 << k))) return false;
    const uint32_t pb = d.ublock[UB_PROJ], mb = d.ublock[UB_MV], tb = d.ublock[UB_TEXM];
    const size_t un = g_buf.uni.size();
    if (pb == kNoBlock || mb == kNoBlock || tb == kNoBlock || static_cast<size_t>(pb) + 16 > un || static_cast<size_t>(mb) + 12 > un ||
        static_cast<size_t>(tb) + static_cast<size_t>(coord) * 12 + 8 > un)
        return false;
    const float* P = g_buf.uni.data() + pb; // (column-major)
    const float* M = g_buf.uni.data() + mb; // (rows)
    const float* T = g_buf.uni.data() + tb + coord * 12;
    const RenderState& rs = d.rs;
    if (!(rs.vpW > 0.0f) || !(rs.vpH > 0.0f)) return false;
    const VtxLayout L = MakeLayout(d.layoutMask);
    size_t base = d.vtxOffset + static_cast<size_t>(d.vtxFirst) * L.stride;
    if (!g_buf.vtxChunks.empty()) {
        if (d.vchunk >= g_buf.vtxChunks.size()) return false;
        base += g_buf.vtxChunks[d.vchunk];
    }
    if (base + static_cast<size_t>(d.vtxCount) * L.stride > g_buf.vtx.size()) return false;
    double X[64], Y[64], S[64], Tt[64], w0 = 0;
    for (uint32_t i = 0; i < d.vtxCount; ++i) {
        const uint8_t* v = g_buf.vtx.data() + base + static_cast<size_t>(i) * L.stride;
        float p[3], uv[2];
        std::memcpy(p, v, 12);
        std::memcpy(uv, v + L.offTex[k], 8);
        double e[3];
        for (int r = 0; r < 3; ++r) e[r] = static_cast<double>(M[r * 4]) * p[0] + static_cast<double>(M[r * 4 + 1]) * p[1] + static_cast<double>(M[r * 4 + 2]) * p[2] + M[r * 4 + 3];
        const double cx = P[0] * e[0] + P[4] * e[1] + P[8] * e[2] + P[12];
        const double cy = P[1] * e[0] + P[5] * e[1] + P[9] * e[2] + P[13];
        const double cw = P[3] * e[0] + P[7] * e[1] + P[11] * e[2] + P[15];
        if (!(cw > 0.0) || !std::isfinite(cx) || !std::isfinite(cy)) return false;
        if (i == 0) w0 = cw;
        else if (std::fabs(cw - w0) > 1e-6 * w0) return false; // (equal w: the coordinates are interpolated affinely on the screen)
        // (u_proj has Y flipped: EFB row = vpY + (y / w + 1) vpH / 2, the back end's glViewport)
        X[i] = rs.vpX + (cx / cw + 1.0) * 0.5 * rs.vpW;
        Y[i] = rs.vpY + (cy / cw + 1.0) * 0.5 * rs.vpH;
        S[i] = T[0] * static_cast<double>(uv[0]) + T[1] * static_cast<double>(uv[1]) + T[2] + T[3];
        Tt[i] = T[4] * static_cast<double>(uv[0]) + T[5] * static_cast<double>(uv[1]) + T[6] + T[7];
        if (!std::isfinite(X[i]) || !std::isfinite(Y[i]) || !std::isfinite(S[i]) || !std::isfinite(Tt[i]) || std::fabs(X[i]) > 16384.0 || std::fabs(Y[i]) > 16384.0)
            return false;
    }
    // s and t as affine functions of (X, Y): from the vertex triple of the largest area, checked at every vertex.
    int ia = 0, ib = 1, ic = 2;
    double best = 0;
    for (uint32_t a = 0; a < d.vtxCount; ++a)
        for (uint32_t b = a + 1; b < d.vtxCount; ++b)
            for (uint32_t c = b + 1; c < d.vtxCount; ++c) {
                const double ar = std::fabs((X[b] - X[a]) * (Y[c] - Y[a]) - (X[c] - X[a]) * (Y[b] - Y[a]));
                if (ar > best) { best = ar; ia = static_cast<int>(a); ib = static_cast<int>(b); ic = static_cast<int>(c); }
            }
    if (!(best > 1e-3)) return false;
    double fs3[3], ft3[3]; // (value = f[0] X + f[1] Y + f[2])
    auto solve = [&](const double* V, double* f) {
        const double det = (X[ib] - X[ia]) * (Y[ic] - Y[ia]) - (X[ic] - X[ia]) * (Y[ib] - Y[ia]);
        f[0] = ((V[ib] - V[ia]) * (Y[ic] - Y[ia]) - (V[ic] - V[ia]) * (Y[ib] - Y[ia])) / det;
        f[1] = ((X[ib] - X[ia]) * (V[ic] - V[ia]) - (X[ic] - X[ia]) * (V[ib] - V[ia])) / det;
        f[2] = V[ia] - f[0] * X[ia] - f[1] * Y[ia];
    };
    solve(S, fs3);
    solve(Tt, ft3);
    double bx0 = 1e30, by0 = 1e30, bx1 = -1e30, by1 = -1e30;
    for (uint32_t i = 0; i < d.vtxCount; ++i) {
        if (std::fabs(fs3[0] * X[i] + fs3[1] * Y[i] + fs3[2] - S[i]) > 1e-5 * (1.0 + std::fabs(S[i]))) return false;
        if (std::fabs(ft3[0] * X[i] + ft3[1] * Y[i] + ft3[2] - Tt[i]) > 1e-5 * (1.0 + std::fabs(Tt[i]))) return false;
        bx0 = std::min(bx0, X[i]); bx1 = std::max(bx1, X[i]); by0 = std::min(by0, Y[i]); by1 = std::max(by1, Y[i]);
    }
    // The draw's screen box clipped by the four half-planes s >= s0, s <= s1, t >= t0, t <= t1 (Sutherland-Hodgman, convex).
    double px[16] = {bx0 - 1, bx1 + 1, bx1 + 1, bx0 - 1}, py[16] = {by0 - 1, by0 - 1, by1 + 1, by1 + 1};
    int n = 4;
    auto clip = [&](const double* f, double lim, double sign) { // keep sign * (f(X, Y) - lim) >= 0
        double qx[16], qy[16];
        int m = 0;
        for (int i = 0; i < n; ++i) {
            const int j = (i + 1) % n;
            const double di = sign * (f[0] * px[i] + f[1] * py[i] + f[2] - lim), dj = sign * (f[0] * px[j] + f[1] * py[j] + f[2] - lim);
            if (di >= 0) { qx[m] = px[i]; qy[m] = py[i]; ++m; }
            if ((di >= 0) != (dj >= 0)) {
                const double u = di / (di - dj);
                qx[m] = px[i] + u * (px[j] - px[i]); qy[m] = py[i] + u * (py[j] - py[i]); ++m;
            }
        }
        n = m;
        for (int i = 0; i < m; ++i) { px[i] = qx[i]; py[i] = qy[i]; }
    };
    if (std::isfinite(s0)) clip(fs3, s0, 1.0);
    if (n && std::isfinite(s1)) clip(fs3, s1, -1.0);
    if (n && std::isfinite(t0)) clip(ft3, t0, 1.0);
    if (n && std::isfinite(t1)) clip(ft3, t1, -1.0);
    if (n == 0) { none = true; return true; }
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    for (int i = 0; i < n; ++i) { x0 = std::min(x0, px[i]); x1 = std::max(x1, px[i]); y0 = std::min(y0, py[i]); y1 = std::max(y1, py[i]); }
    // (a pixel's sample is at its centre: whole pixels around the window, one more on each side for the scissor's rounding)
    const int ix0 = static_cast<int>(std::floor(x0)) - 1, iy0 = static_cast<int>(std::floor(y0)) - 1;
    const int ix1 = static_cast<int>(std::ceil(x1)) + 1, iy1 = static_cast<int>(std::ceil(y1)) + 1;
    out = EfbRect{static_cast<int16_t>(ix0), static_cast<int16_t>(iy0), static_cast<int16_t>(ix1 - ix0), static_cast<int16_t>(iy1 - iy0)};
    return true;
}
void SfLog() {
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::seconds(60) || !g_sfCtr.pictures) return;
    const double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    const SfCount& c = g_sfCtr;
    const uint64_t pairs = c.skipped + c.scissored + c.full;
    char line[400];
    std::snprintf(line, sizeof line,
                  "shadowfill: %.0f s, %llu pictures, %llu with the silhouette fill: left out %llu, scissored %llu (mean %.1f%% of the screen), full %llu; "
                  "captures made a fill of 0: %llu (%llu needed none: the texture held it); shrink: %llu moved (mean %.1f%% of the screen), %llu refused",
                  secs, static_cast<unsigned long long>(c.pictures), static_cast<unsigned long long>(pairs), static_cast<unsigned long long>(c.skipped),
                  static_cast<unsigned long long>(c.scissored), c.scissored ? 100.0 * c.area / static_cast<double>(c.scissored) : 0.0,
                  static_cast<unsigned long long>(c.full), static_cast<unsigned long long>(c.fills), static_cast<unsigned long long>(c.fillsKept),
                  static_cast<unsigned long long>(c.shrunk), c.shrunk ? 100.0 * c.shrinkArea / static_cast<double>(c.shrunk) : 0.0, static_cast<unsigned long long>(c.shrinkFail));
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    g_sfTotal.pictures += c.pictures; g_sfTotal.skipped += c.skipped; g_sfTotal.scissored += c.scissored; g_sfTotal.full += c.full;
    g_sfTotal.fills += c.fills; g_sfTotal.fillsKept += c.fillsKept; g_sfTotal.area += c.area;
    g_sfTotal.shrunk += c.shrunk; g_sfTotal.shrinkFail += c.shrinkFail; g_sfTotal.shrinkArea += c.shrinkArea;
    g_sfCtr = SfCount{};
    if (ShadowRectFeMode() != 0) {
        const SfFeCount& f = g_sfFe;
        char l2[300];
        std::snprintf(l2, sizeof l2,
                      "shadowrect-fe: GXV_SHADOW_RECT_FE=%d, since the start: bounds from the memo %llu, worked out %llu (%llu points, %llu repeated vertices skipped), "
                      "deferred key copies not made %llu; compared with the plain code %llu, differing %llu",
                      ShadowRectFeMode(), static_cast<unsigned long long>(f.hits), static_cast<unsigned long long>(f.misses),
                      static_cast<unsigned long long>(f.points), static_cast<unsigned long long>(f.dupSkipped), static_cast<unsigned long long>(f.keysSkipped),
                      static_cast<unsigned long long>(f.verified), static_cast<unsigned long long>(f.differing));
        if (port_log_summary) port_log_summary("%s", l2);
        else std::fprintf(stderr, "[vgx] %s\n", l2);
    }
}
// ================================================================ shadow rectangle (GXV_SHADOW_RECT, shadowpx23)
// The silhouette / shadow pass every picture (MR::fillSilhouetteColor, then MarioActor::drawShadow): ScreenAlphaCapture copies the WHOLE
// EFB's alpha (GX_CTF_A8, 640x456 -> a render-size texture: a copy scene of its own, the EFB scene split around it), the fill quad
// samples it over the whole screen. Only the pixels in N (above: where alpha may be other than 0) can give a texel other than 0, and N
// is the shadow volumes, the player's silhouette and Mario's corner: a fraction of the screen. GXV_SHADOWFILL_BOUNDS already cuts the
// FILL to the pixels whose samples can reach N (or leaves out fill and copy when N is empty); the copy itself was always whole, and
// with a non-empty N (every picture with Mario on screen) the copy scene's pixel work stays. Here the capture is marked with N's
// bounding box inside its source (CopyTexCmd::nz); the back end (CopyTex, "GXV_SHADOW_RECT") copies only that box, plus the copy's
// bilinear margin, united with whatever its own last copy into the same texture storage may have left other than 0, and keeps the
// rest of the texture - which holds 0 there, and 0 is what the whole copy would write there. So the texture is the whole copy, bit
// for bit, for every reader (the fill, GXV_HOIST_GRID's corner pass, anything later); no reader has to be proven to sample only there.
//   GXV_SHADOW_RECT=0 (default) off; 1 on (also turns on GXV_SHADOWFILL_BOUNDS's fill cuts); 2 N is worked out and the "shadowrect:"
//   line logged, nothing changes. Read once. Basic log about once a minute: "shadowrect: ..." (here) and "shadowrect (back end): ...".
struct SrCount {
    uint64_t pictures = 0, captures = 0, marked = 0, whole = 0, wholeAll = 0, wholeUnknown = 0, wholeShape = 0, zero = 0;
    uint64_t writers = 0, unknown = 0, unknownArea = 0;
    double area = 0, areaMin = 1e9, areaMax = 0;
} g_srCtr, g_srTotal;
uint32_t g_srUnknownPic = 0; // alpha writers of unknown pixels (DrawRec::ab not worked out: their extent counts) this picture so far
void SrPictureBegin() {
    ++g_srCtr.pictures;
    g_srUnknownPic = 0;
}
void SrNoteWriter(const DrawRec& d) {
    ++g_srCtr.writers;
    if (!d.ab) {
        ++g_srCtr.unknown;
        ++g_srUnknownPic;
    }
}
// A capture that reads only the EFB's alpha (ShadowFillPass's test): marked with N's box `b` when it is whole-EFB 1:1 and adds nothing.
void SrCapture(CopyTexCmd& t, bool zero, const EfbRect& b, bool mark) {
    ++g_srCtr.captures;
    if (zero) { ++g_srCtr.zero; return; } // (GXV_SHADOWFILL_BOUNDS's case: a fill of 0, or nothing)
    bool finite = true;
    for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 4; ++col) finite = finite && std::isfinite(t.mat[r][col]);
    const bool shape = t.src.x == 0 && t.src.y == 0 && t.src.w == static_cast<int>(g.efbW) && t.src.h == static_cast<int>(g.efbH) && t.dstW == t.src.w &&
                       t.dstH == t.src.h && !t.dstTexW && finite && t.add[0] == 0 && t.add[1] == 0 && t.add[2] == 0 && t.add[3] == 0;
    const double frac = RArea(t.src) > 0 ? static_cast<double>(RArea(b)) / static_cast<double>(RArea(t.src)) : 1.0;
    if (!shape) { ++g_srCtr.whole; ++g_srCtr.wholeShape; return; }
    if (frac >= 0.999) {
        ++g_srCtr.whole;
        ++g_srCtr.wholeAll;
        if (g_srUnknownPic) ++g_srCtr.wholeUnknown;
    }
    ++g_srCtr.marked;
    g_srCtr.area += frac;
    g_srCtr.areaMin = std::min(g_srCtr.areaMin, frac);
    g_srCtr.areaMax = std::max(g_srCtr.areaMax, frac);
    if (g_srUnknownPic) ++g_srCtr.unknownArea;
    if (!mark) return; // (=2: logged only)
    t.nzOn = 1;
    t.nz = b;
}
void SrLog() {
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::seconds(60) || !g_srCtr.pictures) return;
    const double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    const SrCount& c = g_srCtr;
    const double np = static_cast<double>(c.pictures), nm = static_cast<double>(std::max<uint64_t>(c.marked, 1));
    char line[512];
    std::snprintf(line, sizeof line,
                  "shadowrect: %.0f s, %llu pictures, %llu alpha captures (%llu of nothing); marked with their rectangle %llu: mean %.1f%% of the EFB "
                  "(min %.1f%%, max %.1f%%), of which the whole EFB %llu (%llu in pictures with an alpha writer of unknown pixels); not marked %llu (shape); "
                  "alpha writers a picture %.1f, of unknown pixels %.2f (GXV_SHADOW_RECT=%d)",
                  secs, static_cast<unsigned long long>(c.pictures), static_cast<unsigned long long>(c.captures), static_cast<unsigned long long>(c.zero),
                  static_cast<unsigned long long>(c.marked), 100.0 * c.area / nm, c.marked ? 100.0 * c.areaMin : 0.0, 100.0 * c.areaMax,
                  static_cast<unsigned long long>(c.wholeAll), static_cast<unsigned long long>(c.wholeUnknown), static_cast<unsigned long long>(c.wholeShape),
                  c.writers / np, c.unknown / np, ShadowRectMode());
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    g_srTotal.pictures += c.pictures; g_srTotal.captures += c.captures; g_srTotal.marked += c.marked; g_srTotal.wholeAll += c.wholeAll;
    g_srTotal.wholeUnknown += c.wholeUnknown; g_srTotal.wholeShape += c.wholeShape; g_srTotal.area += c.area; g_srTotal.zero += c.zero;
    g_srCtr = SrCount{};
}
} // namespace
void ShadowRectCounters(uint64_t out[6]) {
    out[0] = g_srTotal.marked + g_srCtr.marked;
    out[1] = g_srTotal.wholeAll + g_srCtr.wholeAll;
    out[2] = g_srTotal.wholeUnknown + g_srCtr.wholeUnknown;
    out[3] = g_srTotal.wholeShape + g_srCtr.wholeShape;
    out[4] = static_cast<uint64_t>((g_srTotal.area + g_srCtr.area) * 1000.0 + 0.5);
    out[5] = g_srTotal.zero + g_srCtr.zero;
}
namespace {
// Reach of a capture's non-zero texels on the screen, EFB pixels per unit of the copy's scale (src / dst): the copy's own bilinear
// read and the fill's bilinear footprint, each a render pixel or a texel, at render scales down to 0.66 of the EFB.
constexpr double kSfReach = 2.0;
// A scissored fill must cover at most this share of its own scissor's pixels: vitaGL redraws its scissor mask at every scissor change
// (tests.c update_scissor_test: about two ISP-only passes over the target, on and off again, ~0.1-0.2 ms each at 960x544), against a fill
// pass of ~1 ms over the whole screen. Above it the fill is left whole (a cost decision; either is exact).
constexpr double kSfMaxArea = 0.6;
void ShadowFillPass() {
    g_sfZeroCopy.assign(g_buf.copyTex.size(), 0);
    const bool fillOn = ShadowFillOn();
    if (!SfBoundsOn()) { g_sfInit = false; return; } // (fix47: GXV_CORNERPASS alone runs the bookkeeping too; it was skipped whole on the console, where ShadowFillOn() is false. cp50: pictures
                                                      // that skip it (auto-off) leave N stale: it starts again from "everything" when the bookkeeping comes back)
    if (!g_sfInit) { g_sfN.All(); g_sfInit = true; }
    g_sfCaps.clear();
    if (fillOn) ++g_sfCtr.pictures;
    const bool cuts = ShadowFillCuts(); // (GXV_SHADOW_RECT=2 alone: N is worked out and logged, nothing is changed)
    const int rectMode = ShadowRectMode();
    const int shrinkMode = ShadowFetchMode() ? 0 : ShadowFillShrinkMode(); // (the fetch leaves the capture out: nothing to move)
    if (rectMode) SrPictureBegin();
    std::vector<Cmd>& cmds = g_buf.cmds;
    size_t out = 0;
    const int cornerMode = CornerPassActive() ? CornerPassMode() : 0; // (GXV_CORNERPASS, imp45; cp50: 0 while the stage has it switched off)
    const bool cornerOnly = cornerMode && !fillOn; // (cp50: N is read over the corner rectangle alone)
    CornerTrack& ct = g_cornerTrack;
    if (cornerMode) ct.Begin();
    for (size_t i = 0; i < cmds.size(); ++i) {
        const Cmd c = cmds[i];
        bool keep = true;
        switch (c.type) {
        case CMD_DRAW: {
            DrawRec& d = g_buf.draws[c.arg];
            // (d) the read of a capture
            if (cuts && d.texMask && !g_sfCaps.empty()) {
                int map = -1, coord = -1;
                for (size_t q = 0; q < g_sfCaps.size(); ++q) {
                    const SfCap& cap = g_sfCaps[q];
                    bool reads = false;
                    for (int m = 0; m < 8; ++m)
                        if ((d.texMask & (1u << m)) && d.tex[m].texId == cap.texId) reads = true;
                    if (!reads) continue;
                    const SfCap cp = cap;
                    g_sfCaps.erase(g_sfCaps.begin() + static_cast<std::ptrdiff_t>(q)); // (only its first reader is looked at)
                    if (!SfFillShape(d, map, coord) || d.tex[map].texId != cp.texId) break;
                    if (cp.zero) {
                        keep = false;
                        ++g_sfCtr.skipped;
                        break;
                    }
                    const TexBind& tbd = d.tex[map];
                    bool none = false;
                    EfbRect win{};
                    bool done = false;
                    if (tbd.wrapS == WRAP_CLAMP && tbd.wrapT == WRAP_CLAMP && tbd.minFilter <= FILT_LINEAR && tbd.magFilter <= FILT_LINEAR &&
                        cp.src.w > 0 && cp.src.h > 0 && cp.dstW > 0 && cp.dstH > 0) {
                        const double rx = kSfReach * (1.0 + static_cast<double>(cp.src.w) / cp.dstW) + 1.0;
                        const double ry = kSfReach * (1.0 + static_cast<double>(cp.src.h) / cp.dstH) + 1.0;
                        const double inf = std::numeric_limits<double>::infinity();
                        // (beyond the source's edge a clamped sample reads the edge texel: no limit on that side)
                        const double s0 = cp.b.x - rx <= cp.src.x ? -inf : (cp.b.x - rx - cp.src.x) / cp.src.w;
                        const double s1 = cp.b.x + cp.b.w + rx >= cp.src.x + cp.src.w ? inf : (cp.b.x + cp.b.w + rx - cp.src.x) / cp.src.w;
                        const double t0 = cp.b.y - ry <= cp.src.y ? -inf : (cp.b.y - ry - cp.src.y) / cp.src.h;
                        const double t1 = cp.b.y + cp.b.h + ry >= cp.src.y + cp.src.h ? inf : (cp.b.y + cp.b.h + ry - cp.src.y) / cp.src.h;
                        if (SfFillWindow(d, coord, s0, s1, t0, t1, none, win)) {
                            RenderState& rs = d.rs;
                            const int x0 = std::max<int>(rs.scX, win.x), y0 = std::max<int>(rs.scY, win.y);
                            const int x1 = std::min<int>(rs.scX + rs.scW, win.x + win.w), y1 = std::min<int>(rs.scY + rs.scH, win.y + win.h);
                            if (none || x1 <= x0 || y1 <= y0) {
                                keep = false;
                                ++g_sfCtr.skipped;
                                done = true;
                            } else if (shrinkMode && (x1 - x0 < rs.scW || y1 - y0 < rs.scH) && SfShrinkFill(d, coord, EfbRect{static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(x1 - x0), static_cast<int16_t>(y1 - y0)})) {
                                ++g_sfCtr.shrunk;
                                g_sfCtr.shrinkArea += static_cast<double>(x1 - x0) * (y1 - y0) / (static_cast<double>(rs.scW) * rs.scH);
                                done = true;
                            } else if (shrinkMode == 2) {
                                ++g_sfCtr.shrinkFail;
                            } else if ((x0 > rs.scX || y0 > rs.scY || x1 < rs.scX + rs.scW || y1 < rs.scY + rs.scH) &&
                                       static_cast<double>(x1 - x0) * (y1 - y0) <= kSfMaxArea * rs.scW * rs.scH) {
                                const double before = static_cast<double>(rs.scW) * rs.scH;
                                rs.scX = static_cast<int16_t>(x0); rs.scY = static_cast<int16_t>(y0);
                                rs.scW = static_cast<int16_t>(x1 - x0); rs.scH = static_cast<int16_t>(y1 - y0);
                                const EfbRect sc{rs.scX, rs.scY, rs.scW, rs.scH};
                                const int ex0 = std::max<int>(d.exX, sc.x), ey0 = std::max<int>(d.exY, sc.y);
                                const int ex1 = std::min<int>(d.exX + d.exW, sc.x + sc.w), ey1 = std::min<int>(d.exY + d.exH, sc.y + sc.h);
                                d.exX = static_cast<int16_t>(ex0); d.exY = static_cast<int16_t>(ey0);
                                d.exW = static_cast<int16_t>(ex1 > ex0 ? ex1 - ex0 : 0); d.exH = static_cast<int16_t>(ey1 > ey0 ? ey1 - ey0 : 0);
                                d.cover = 0;
                                ++g_sfCtr.scissored;
                                g_sfCtr.area += before > 0 ? static_cast<double>(sc.w) * sc.h / before : 0.0;
                                done = true;
                            }
                        }
                    }
                    if (!done) ++g_sfCtr.full;
                    break;
                }
            }
            if (!keep) break;
            const int kind = SfAlphaKind(d);
            if (kind == 2) {
                EfbRect a = d.ab ? EfbRect{d.abX, d.abY, d.abW, d.abH} : EfbRect{d.exX, d.exY, d.exW, d.exH};
                if (cornerOnly && !d.ab) { // (cp50: no bounds were worked out (or none known): the extent cut to the scissor, which holds every pixel the draw writes)
                    const int x0 = std::max<int>(a.x, d.rs.scX), y0 = std::max<int>(a.y, d.rs.scY);
                    const int x1 = std::min<int>(a.x + a.w, d.rs.scX + d.rs.scW), y1 = std::min<int>(a.y + a.h, d.rs.scY + d.rs.scH);
                    a = EfbRect{static_cast<int16_t>(x0), static_cast<int16_t>(y0), static_cast<int16_t>(std::max(0, x1 - x0)), static_cast<int16_t>(std::max(0, y1 - y0))};
                }
                if (!cornerOnly || !ct.have || Overlap(a, ct.rm)) g_sfN.Add(a); // (cp50: corner alone: N matters over the rectangle only)
                if (cornerMode) ct.Writer(a, c.arg);
            } else if (kind == 1 && (d.cover || d.coverCull)) {
                g_sfN.Sub(EfbRect{d.exX, d.exY, d.exW, d.exH});
                if (cornerMode) ct.Recheck();
            }
            if (kind == 2 && rectMode) SrNoteWriter(d);
            // (fix47, the fill's certificate above) a certified fill that writes the constant 0, cannot fail a depth test and whose scissor holds the corner and its margin
            if (cornerMode && kind == 1 && ct.have && !g_ssZeroFill.empty() && SsZeroFill(c.arg) && (!d.rs.depthTest || (d.rs.depthFunc & 7) == CMP_ALWAYS) &&
                Inside(ct.rm, EfbRect{d.rs.scX, d.rs.scY, d.rs.scW, d.rs.scH})) {
                ct.ZeroByFill();
                ++ct.fillsCertified;
            }
            break;
        }
        case CMD_COPY_TEX: {
            CopyTexCmd& t = g_buf.copyTex[c.arg];
            for (size_t q = 0; q < g_sfCaps.size(); ++q)
                if (g_sfCaps[q].texId == t.texId) { g_sfCaps.erase(g_sfCaps.begin() + static_cast<std::ptrdiff_t>(q)); break; }
            // (GXV_CORNERPASS, imp45) a small alpha-only copy of the EFB, into a texture of its own: marked when the alpha it reads is 0 plus this picture's draws
            if (cornerMode && !t.depth && !t.srcTex && !t.skip && !t.fill && !t.fromPass && !t.passMore && !t.dstTexW && t.texId && t.src.w > 0 && t.src.h > 0 &&
                t.src.x >= 0 && t.src.y >= 0 && t.src.x + t.src.w <= g.efbW && t.src.y + t.src.h <= g.efbH && (t.src.w < g.efbW || t.src.h < g.efbH) && CopyReads(t) == CH_A) {
                ++ct.seen;
                const EfbRect rm = CornerMargin(t.src);
                const bool same = ct.have && rm.x == ct.rm.x && rm.y == ct.rm.y && rm.w == ct.rm.w && rm.h == ct.rm.h && t.src.x == ct.src.x && t.src.y == ct.src.y &&
                                  t.src.w == ct.src.w && t.src.h == ct.src.h;
                if (!same) {
                    ++ct.whyRect;
                    ct.rm = rm;
                    ct.src = t.src;
                    ct.have = true;
                    g_cpRm.rm = rm; g_cpRm.have = true;
                    if (cornerOnly) g_sfN.All(); // (cp50: draws away from the old rectangle were not added to N: nothing is known over the new one)
                    ct.Unknown(false); // (what N held there before this point was not followed)
                } else if (ct.state == CornerTrack::WRITING && ct.first) {
                    t.cornerPass = static_cast<uint8_t>(cornerMode);
                    t.cornerFrom = ct.first;
                    t.cornerFill = ct.byFill ? 1 : 0;
                    if (ct.byFill) ++ct.byFillMarked;
                    ++ct.marked;
                } else if (ct.state == CornerTrack::ZERO) {
                    ++ct.whyNoWriter;
                } else if (ct.targetChanged) {
                    ++ct.whyTarget;
                } else {
                    ++ct.whyAlpha;
                }
            }
            if (!t.depth && !t.srcTex && !t.skip && !t.fill && !t.fromPass && t.src.w > 0 && t.src.h > 0 && CopyReads(t) == CH_A) {
                EfbRect b{};
                const bool zero = !g_sfN.Bound(t.src, b);
                if (zero && cuts) g_sfZeroCopy[c.arg] = 1; // (IsolatePasses: the source's alpha is known to be 0)
                if (!t.dstTexW && t.add[0] == 0 && t.add[1] == 0 && t.add[2] == 0 && t.add[3] == 0 && g_sfCaps.size() < 8)
                    g_sfCaps.push_back(SfCap{t.texId, zero, b, t.src, t.dstW, t.dstH});
                if (rectMode) SrCapture(t, zero, b, rectMode == 1);
            }
            if ((t.clear.enable || t.clearElided) && t.clear.alpha) {
                if (t.clear.rgba[3] == 0.0f) { g_sfN.Sub(t.src); if (cornerMode) { if (ct.have && Overlap(t.src, ct.rm)) ct.Unknown(false); ct.Recheck(); } } // (a clear is not a draw: WRITING ends; ZERO if nothing is left)
                else { g_sfN.Add(t.src); if (cornerMode && ct.have && Overlap(t.src, ct.rm)) ct.Unknown(false); } // (a clear is not a draw the pass could make again)
            }
            break;
        }
        case CMD_COPY_DISP: {
            const CopyDispCmd& dc = g_buf.copyDisp[c.arg];
            if ((dc.clear.enable || dc.clearElided) && dc.clear.alpha) {
                if (dc.clear.rgba[3] == 0.0f) { g_sfN.Sub(dc.src); if (cornerMode) { if (ct.have && Overlap(dc.src, ct.rm)) ct.Unknown(false); ct.Recheck(); } }
                else { g_sfN.Add(dc.src); if (cornerMode && ct.have && Overlap(dc.src, ct.rm)) ct.Unknown(false); }
            }
            break;
        }
        case CMD_SET_TARGET:
            g_sfN.All();
            g_sfCaps.clear();
            if (cornerMode) ct.Unknown(true);
            break;
        case CMD_TEX_FREE:
        case CMD_TEX_UPLOAD: {
            const uint32_t id = c.type == CMD_TEX_FREE ? c.arg : g_buf.uploads[c.arg].texId;
            for (size_t q = 0; q < g_sfCaps.size(); ++q)
                if (g_sfCaps[q].texId == id) { g_sfCaps.erase(g_sfCaps.begin() + static_cast<std::ptrdiff_t>(q)); break; }
            break;
        }
        case CMD_PASS_BEGIN: // (made later, by IsolatePasses: not in the frame yet)
            g_sfN.All();
            if (cornerMode) ct.Unknown(true);
            break;
        default:
            break;
        }
        if (keep) cmds[out++] = c;
    }
    cmds.resize(out);
    if (fillOn) SfLog(); // (fix47: no "shadowfill:" line for GXV_CORNERPASS alone)
    if (rectMode) SrLog();
}
} // namespace
// cp50: the back end (PlanCornerPass) says a stage's marked copies came to nothing / a stage has loaded.
void CornerPassAutoOff(int off) { g_cpAutoOff.store(off, std::memory_order_relaxed); }
int CornerPassMode() { return static_cast<int>(VGX_KNOB(static_cast<int>(std::clamp<long>(EnvInt("GXV_CORNERPASS", 0), 0, 2)))); }
void CornerPassFeCounters(uint64_t out[6]) {
    const CornerTrack& c = g_cornerTrack;
    out[0] = c.seen; out[1] = c.marked; out[2] = c.whyRect; out[3] = c.whyAlpha; out[4] = c.whyNoWriter; out[5] = c.whyTarget;
}
void CornerPassFeFill(uint64_t out[2]) { out[0] = g_cornerTrack.byFillMarked; out[1] = g_cornerTrack.fillsCertified; }
void ShadowRectFeCounters(uint64_t out[7]) {
    out[0] = g_sfFe.hits; out[1] = g_sfFe.misses; out[2] = g_sfFe.dupSkipped; out[3] = g_sfFe.points; out[4] = g_sfFe.keysSkipped;
    out[5] = g_sfFe.verified; out[6] = g_sfFe.differing;
}
// (tests) GXV_SHADOWFILL_BOUNDS totals since the start: silhouette fills left out, scissored, left whole; captures made fills of 0,
// and of those the ones not even made (the texture held 0).
void ShadowFillCounters(uint64_t out[5]) {
    out[0] = g_sfTotal.skipped + g_sfCtr.skipped;
    out[1] = g_sfTotal.scissored + g_sfCtr.scissored;
    out[2] = g_sfTotal.full + g_sfCtr.full;
    out[3] = g_sfTotal.fills + g_sfCtr.fills;
    out[4] = g_sfTotal.fillsKept + g_sfCtr.fillsKept;
}

bool PrepassPair(uint16_t fs, uint16_t& orig, uint16_t& colour) {
    if (fs >= kPreMax) return false;
    const uint32_t v = g_preMap[fs].load(std::memory_order_acquire);
    if (!v) return false;
    orig = static_cast<uint16_t>((v >> 16) - 1u);
    colour = static_cast<uint16_t>(v & 0xFFFFu);
    return true;
}
bool PrepassAny() { return g_preAny.load(std::memory_order_relaxed) != 0; }
// (the back end's log line) [0] draws given a prepass key, [1] keys made, [2] refused
void PrepassCounters(uint64_t out[9]) { out[0] = g_preCtr.draws; out[1] = g_preCtr.madeKeys; out[2] = g_preCtr.refused; for (int i = 0; i < 6; ++i) out[3 + i] = g_preCtr.why[i]; }
// ================================================================ GXV_SHADOWFETCH (gpu23b): the silhouette fill reads the EFB's alpha itself
// MR::fillSilhouetteColor (see "shadow fill bounds" above) copies the whole EFB's alpha into a texture (GX_CTF_A8) and at once draws a
// screen quad that samples it texel for pixel. On the console the copy splits the EFB's scene: the EFB's colour and depth stored, a
// copy scene, the EFB loaded again (TA 0.5-0.9 ms, 3D 1-1.5 ms at 960x544), every picture of every galaxy. The SGX blends in its
// fragment programs, and a GXM fragment program may read the colour surface's pixel itself (programmable blending: a __nativecolor
// Cg program with a FRAGCOLOR input; vitaGL patch 0025, vglProgramNativeColor). Then the fill needs no copy: per pixel it reads the
// EFB's alpha a, and where a > T writes f(colour) and the constant alpha the blend leaves, elsewhere the pixel as it is.
// This pass finds the pair - a copy that reads only the EFB's alpha, 1:1 from the whole EFB, no clear; the next command that draws is
// the one quad that samples it (the shape ShadowFillPass knows: one TEV stage "TEXA > b ? c : 0" with a constant colour and c, an
// alpha test that fails for 0, clamped bilinear or nearest sampling, a blend whose factors do not read the destination); its texture
// coordinate maps EFB pixel (X, Y) to (X / W, Y / H) exactly (an axis-aligned rectangle of two triangles, equal w); nothing else in
// the picture reads the texture before it is written again - and gives the back end the fill's key (program, TEV block, blend, copy).
// The back end checks the fetch on the console itself before it is ever used (vgx_backend_gl.cpp "shadow fetch": the real fill and
// the fetch fill drawn over a test pattern that holds every byte in every channel against every alpha, read back, compared pixel
// for pixel, and the fetch's T and f found from the real fill's pixels); only a key it found exact is fetched (fetchMode 2: the copy
// left out). Pictures are therefore the same, bit for bit, or the switch does nothing.
//   GXV_SHADOWFETCH=0 (default) off; 1 on; 2 calibrate and check only (the copies are all made; the log says whether 1 would be
//   exact). Read once. Basic log about once a minute: "shadowfetch: ..." (here: pairs, fetched, calibration requests, stale reads;
//   the back end: fills drawn by the fetch, fallbacks, calibrations).
int ShadowFetchMode() {
    const int m = static_cast<int>(VGX_KNOB(std::clamp<long>(EnvInt("GXV_SHADOWFETCH", 0), 0, 2)));
    return m;
}
namespace {
struct SfeSlot { std::atomic<uint64_t> key{0}; std::atomic<int> state{0}; };
SfeSlot g_sfeSlots[16];
std::atomic<int> g_sfeUsable{0};
int SfeState(uint64_t key) {
    for (SfeSlot& s : g_sfeSlots)
        if (s.key.load(std::memory_order_acquire) == key) return s.state.load(std::memory_order_acquire);
    return 0;
}
struct SfeCount { uint64_t pictures = 0, pairs = 0, fetched = 0, requests = 0, stale = 0; } g_sfeCtr, g_sfeTotal;
std::vector<uint32_t> g_sfeStale; // textures whose last capture was left out (fetched): nothing may read them before the next write
int g_sfeEfbW = 0, g_sfeEfbH = 0;
inline uint64_t SfeMix(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; }
// The fill's geometry: an axis-aligned rectangle (two triangles that tile it) whose coordinate on `coord` is (X / efbW, Y / efbH) at
// every vertex (with equal w both are affine on the screen, so at every pixel); rect = its corners in clip space (x / w, y / w).
bool SfeFillGeom(const DrawRec& d, int coord, int efbW, int efbH, float rect[4], uint64_t& vhash) {
    if (d.vbuf || d.prim != PRIM_TRIANGLES || d.vtxCount != 4 || d.idxCount != 6 || efbW <= 0 || efbH <= 0) return false;
    if (d.vsId >= g_vsReg.keys.size() || d.fsId >= g_fsReg.keys.size()) return false;
    const VsKey& vs = g_vsReg.keys[d.vsId];
    const FsKey& fs = g_fsReg.keys[d.fsId];
    if (vs.perVtxPos || vs.lpMode() || coord >= vs.numTexGens || (fs.texProjMask & (1u << coord))) return false;
    const VsKey::TexGen& tg = vs.tg[coord];
    if (tg.type != 0 || tg.src < 5 || tg.src > 12 || tg.post || tg.perVtxMtx || tg.proj) return false;
    const int k = tg.src - 5;
    if (!(d.layoutMask & (VL_TEX0 << k))) return false;
    const uint32_t pb = d.ublock[UB_PROJ], mb = d.ublock[UB_MV], tb = d.ublock[UB_TEXM];
    const size_t un = g_buf.uni.size();
    if (pb == kNoBlock || mb == kNoBlock || tb == kNoBlock || static_cast<size_t>(pb) + 16 > un || static_cast<size_t>(mb) + 12 > un ||
        static_cast<size_t>(tb) + static_cast<size_t>(coord) * 12 + 8 > un)
        return false;
    const float* P = g_buf.uni.data() + pb;
    const float* M = g_buf.uni.data() + mb;
    const float* T = g_buf.uni.data() + tb + coord * 12;
    const RenderState& rs = d.rs;
    // (the viewport is the whole EFB and the quad covers it: the fill reaches every pixel its scissor lets through)
    if (rs.vpX != 0.0f || rs.vpY != 0.0f || rs.vpW != static_cast<float>(efbW) || rs.vpH != static_cast<float>(efbH)) return false;
    const VtxLayout L = MakeLayout(d.layoutMask);
    size_t base = d.vtxOffset + static_cast<size_t>(d.vtxFirst) * L.stride;
    if (!g_buf.vtxChunks.empty()) {
        if (d.vchunk >= g_buf.vtxChunks.size()) return false;
        base += g_buf.vtxChunks[d.vchunk];
    }
    if (base + 4 * static_cast<size_t>(L.stride) > g_buf.vtx.size()) return false;
    if (static_cast<size_t>(d.idxOffset) + 6 > g_buf.idx.size()) return false;
    // (vertex colours - FoldRegs' TEV registers from the vertices, FsKey::regFromVtx - the same at every corner: a constant; the
    // key holds every byte of the four vertices)
    vhash = 14695981039346656037ull;
    for (size_t i = 0; i < 4 * static_cast<size_t>(L.stride); ++i) vhash = SfeMix(vhash, g_buf.vtx[base + i]);
    for (int i = 1; i < 4; ++i) {
        const uint8_t* v0 = g_buf.vtx.data() + base;
        const uint8_t* v = v0 + static_cast<size_t>(i) * L.stride;
        if ((d.layoutMask & VL_COL0) && std::memcmp(v0 + L.offCol0, v + L.offCol0, 4) != 0) return false;
        if ((d.layoutMask & VL_COL1) && std::memcmp(v0 + L.offCol1, v + L.offCol1, 4) != 0) return false;
    }
    double nx[4], ny[4], w0 = 0;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* v = g_buf.vtx.data() + base + static_cast<size_t>(i) * L.stride;
        float p[3], uv[2];
        std::memcpy(p, v, 12);
        std::memcpy(uv, v + L.offTex[k], 8);
        double e[3];
        for (int r = 0; r < 3; ++r) e[r] = static_cast<double>(M[r * 4]) * p[0] + static_cast<double>(M[r * 4 + 1]) * p[1] + static_cast<double>(M[r * 4 + 2]) * p[2] + M[r * 4 + 3];
        const double cx = P[0] * e[0] + P[4] * e[1] + P[8] * e[2] + P[12];
        const double cy = P[1] * e[0] + P[5] * e[1] + P[9] * e[2] + P[13];
        const double cw = P[3] * e[0] + P[7] * e[1] + P[11] * e[2] + P[15];
        if (!(cw > 0.0) || !std::isfinite(cx) || !std::isfinite(cy)) return false;
        if (i == 0) w0 = cw;
        else if (std::fabs(cw - w0) > 1e-6 * w0) return false;
        nx[i] = cx / cw;
        ny[i] = cy / cw;
        const double X = rs.vpX + (nx[i] + 1.0) * 0.5 * rs.vpW, Y = rs.vpY + (ny[i] + 1.0) * 0.5 * rs.vpH;
        const double S = T[0] * static_cast<double>(uv[0]) + T[1] * static_cast<double>(uv[1]) + T[2] + T[3];
        const double Tt = T[4] * static_cast<double>(uv[0]) + T[5] * static_cast<double>(uv[1]) + T[6] + T[7];
        if (!std::isfinite(S) || !std::isfinite(Tt) || std::fabs(nx[i]) > 64.0 || std::fabs(ny[i]) > 64.0) return false;
        // (texel = pixel: the coordinate is the pixel's place in the whole EFB; 1e-6 of a texture is far below a render pixel)
        if (std::fabs(S - X / efbW) > 1e-6 || std::fabs(Tt - Y / efbH) > 1e-6) return false;
    }
    // (an axis-aligned rectangle: two x values and two y values, each corner once)
    const double xa = std::min({nx[0], nx[1], nx[2], nx[3]}), xb = std::max({nx[0], nx[1], nx[2], nx[3]});
    const double ya = std::min({ny[0], ny[1], ny[2], ny[3]}), yb = std::max({ny[0], ny[1], ny[2], ny[3]});
    if (xa > -1.0 + 1e-6 || xb < 1.0 - 1e-6 || ya > -1.0 + 1e-6 || yb < 1.0 - 1e-6) return false;
    int corner[4];
    unsigned seen = 0;
    for (int i = 0; i < 4; ++i) {
        const bool lx = std::fabs(nx[i] - xa) <= 1e-9, hx = std::fabs(nx[i] - xb) <= 1e-9;
        const bool ly = std::fabs(ny[i] - ya) <= 1e-9, hy = std::fabs(ny[i] - yb) <= 1e-9;
        if (lx == hx || ly == hy) return false;
        corner[i] = (hx ? 1 : 0) | (hy ? 2 : 0);
        if (seen & (1u << corner[i])) return false;
        seen |= 1u << corner[i];
    }
    // (two triangles that tile it: each has three different corners and the corners they leave out are opposite)
    int missing[2];
    for (int t = 0; t < 2; ++t) {
        unsigned m = 0;
        for (int q = 0; q < 3; ++q) {
            const uint32_t ix = g_buf.idx[d.idxOffset + t * 3 + q];
            if (ix < d.vtxFirst || ix - d.vtxFirst >= 4) return false;
            m |= 1u << corner[ix - d.vtxFirst];
        }
        if (__builtin_popcount(m) != 3) return false;
        missing[t] = __builtin_ctz(~m & 0xFu);
    }
    if ((missing[0] ^ missing[1]) != 3) return false;
    rect[0] = static_cast<float>(xa); rect[1] = static_cast<float>(ya); rect[2] = static_cast<float>(xb); rect[3] = static_cast<float>(yb);
    return true;
}
// GXV_SHADOWFILL_SHRINK: the fill record `d` (SfFillShape has accepted it) with its four vertices moved to the window `win` (see vgx_fillshrink.h). Proves the shape with SfeFillGeom first
// (the record's own vertices, the whole EFB), then rewrites the position and the coordinate in the frame's vertex arena. False: nothing was written.
bool SfShrinkFill(DrawRec& d, int coord, const EfbRect& win) {
    // (SfeFillGeom wants the viewport to be the whole EFB: its size is the viewport's)
    const int efbW = static_cast<int>(d.rs.vpW + 0.5f), efbH = static_cast<int>(d.rs.vpH + 0.5f);
    float rect[4];
    uint64_t vhash = 0;
    if (!SfeFillGeom(d, coord, efbW, efbH, rect, vhash)) return false;
    const VsKey& vs = g_vsReg.keys[d.vsId];
    const int k = vs.tg[coord].src - 5;
    const VtxLayout L = MakeLayout(d.layoutMask);
    size_t base = d.vtxOffset + static_cast<size_t>(d.vtxFirst) * L.stride;
    if (!g_buf.vtxChunks.empty()) base += g_buf.vtxChunks[d.vchunk];
    fillshrink::Xf xf{g_buf.uni.data() + d.ublock[UB_PROJ], g_buf.uni.data() + d.ublock[UB_MV], g_buf.uni.data() + d.ublock[UB_TEXM] + coord * 12,
                      static_cast<double>(d.rs.vpX), static_cast<double>(d.rs.vpY), static_cast<double>(d.rs.vpW), static_cast<double>(d.rs.vpH)};
    fillshrink::Vtx in[4], out[4];
    for (int i = 0; i < 4; ++i) {
        const uint8_t* v = g_buf.vtx.data() + base + static_cast<size_t>(i) * L.stride;
        std::memcpy(in[i].p, v, 12);
        std::memcpy(in[i].uv, v + L.offTex[k], 8);
    }
    if (!fillshrink::Shrink(xf, efbW, efbH, in, win.x, win.y, win.w, win.h, out)) return false;
    // (the four vertices are this record's own: a dynamic draw of 4 vertices and 6 indices, SfeFillGeom)
    for (int i = 0; i < 4; ++i) {
        uint8_t* v = g_buf.vtx.data() + base + static_cast<size_t>(i) * L.stride;
        std::memcpy(v, out[i].p, 12);
        std::memcpy(v + L.offTex[k], out[i].uv, 8);
    }
    // (what the record can write now: the window; it no longer covers the scissor)
    const int ex0 = std::max<int>(d.exX, win.x), ey0 = std::max<int>(d.exY, win.y);
    const int ex1 = std::min<int>(d.exX + d.exW, win.x + win.w), ey1 = std::min<int>(d.exY + d.exH, win.y + win.h);
    d.exX = static_cast<int16_t>(ex0); d.exY = static_cast<int16_t>(ey0);
    d.exW = static_cast<int16_t>(ex1 > ex0 ? ex1 - ex0 : 0); d.exH = static_cast<int16_t>(ey1 > ey0 ? ey1 - ey0 : 0);
    d.cover = 0;
    d.ab = 0;
    return true;
}
// The fill's own conditions beyond SfFillShape: a constant colour and alpha out of its one stage, no fog, no z texture, no vertex
// registers; a blend that does not read the destination (so each channel's result is a function of that channel alone) and leaves
// alpha the constant of the blend or untouched.
bool SfeFillState(const DrawRec& d, int map) {
    const FsKey& k = g_fsReg.keys[d.fsId];
    if (k.fog || k.ztex || k.logicSrc || k.numTexGens < 1) return false;
    if (d.texMask != (1u << map)) return false;
    const uint32_t c = k.stage[0].color, a = k.stage[0].alpha;
    for (int s = 0; s < 4; ++s) {
        const uint32_t in = bits(c, 4 * s, 4);
        if (in >= 8 && in <= 11) return false; // (TEXC, TEXA, RASC, RASA)
    }
    const uint32_t ac = bits(a, 7, 3);
    if (ac == 4 || ac == 5) return false; // (alpha c: TEXA, RASA)
    const TexBind& tb = d.tex[map];
    if (tb.wrapS != WRAP_CLAMP || tb.wrapT != WRAP_CLAMP || tb.minFilter > FILT_LINEAR || tb.magFilter > FILT_LINEAR) return false;
    const RenderState& rs = d.rs;
    if (!rs.blend || !rs.colorWrite || (rs.blendEq != BE_ADD && rs.blendEq != BE_REVSUB)) return false;
    auto dstFactor = [](uint8_t f) { return f == BF_DST_ALPHA || f == BF_INV_DST_ALPHA || f == BF_DST_COLOR || f == BF_INV_DST_COLOR; };
    if (dstFactor(rs.blendSrc) || dstFactor(rs.blendDst)) return false;
    if (rs.alphaWrite && rs.dstAlphaZero != 1 && rs.dstAlphaZero != 2) return false;
    return true;
}
uint64_t SfeKey(const DrawRec& d, const CopyTexCmd& c, uint64_t vhash) {
    uint64_t h = 14695981039346656037ull;
    h = SfeMix(h, vhash);
    h = SfeMix(h, d.fsId);
    const RenderState& rs = d.rs;
    h = SfeMix(h, static_cast<uint64_t>(rs.blend) | static_cast<uint64_t>(rs.blendSrc) << 8 | static_cast<uint64_t>(rs.blendDst) << 16 |
                      static_cast<uint64_t>(rs.blendEq) << 24 | static_cast<uint64_t>(rs.colorWrite) << 32 | static_cast<uint64_t>(rs.alphaWrite) << 40 |
                      static_cast<uint64_t>(rs.dstAlphaZero) << 48);
    const uint32_t tb = d.ublock[UB_TEV];
    const size_t nTev = static_cast<size_t>(FsTevCount(g_fsReg.keys[d.fsId])) * 4;
    if (tb != kNoBlock && static_cast<size_t>(tb) + nTev <= g_buf.uni.size())
        for (size_t i = 0; i < nTev; ++i) {
            uint32_t w;
            std::memcpy(&w, &g_buf.uni[tb + i], 4);
            h = SfeMix(h, w);
        }
    else h = SfeMix(h, 0xBADull);
    for (int r = 0; r < 4; ++r) {
        for (int col = 0; col < 4; ++col) { uint32_t w; std::memcpy(&w, &c.mat[r][col], 4); h = SfeMix(h, w); }
        uint32_t w;
        std::memcpy(&w, &c.add[r], 4);
        h = SfeMix(h, w);
    }
    return h ? h : 1;
}
void SfeLog(int mode) {
    static auto last = FeClock::Picture();
    const auto now = FeClock::Picture();
    if (now - last < std::chrono::seconds(60) || !g_sfeCtr.pictures) return;
    const double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    const SfeCount& c = g_sfeCtr;
    char line[320];
    std::snprintf(line, sizeof line,
                  "shadowfetch: %.0f s, %llu pictures (GXV_SHADOWFETCH=%d), %llu fill pairs: copy left out (fetched) %llu, calibration requests %llu; "
                  "back end usable %d; stale reads %llu (total %llu)",
                  secs, static_cast<unsigned long long>(c.pictures), mode, static_cast<unsigned long long>(c.pairs), static_cast<unsigned long long>(c.fetched),
                  static_cast<unsigned long long>(c.requests), g_sfeUsable.load(std::memory_order_relaxed), static_cast<unsigned long long>(c.stale),
                  static_cast<unsigned long long>(g_sfeTotal.stale + c.stale));
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    g_sfeTotal.pictures += c.pictures; g_sfeTotal.pairs += c.pairs; g_sfeTotal.fetched += c.fetched; g_sfeTotal.requests += c.requests;
    g_sfeTotal.stale += c.stale;
    g_sfeCtr = SfeCount{};
}
void ShadowFetchPass() {
    const int mode = ShadowFetchMode();
    if (!mode) return;
    ++g_sfeCtr.pictures;
    if (!g_sfeEfbW) { g_sfeEfbW = g.efbW; g_sfeEfbH = g.efbH; }
    std::vector<Cmd>& cmds = g_buf.cmds;
    const size_t n = cmds.size();
    auto staleAt = [&](uint32_t id) -> int {
        for (size_t k = 0; k < g_sfeStale.size(); ++k)
            if (g_sfeStale[k] == id) return static_cast<int>(k);
        return -1;
    };
    auto unstale = [&](int k) { g_sfeStale[k] = g_sfeStale.back(); g_sfeStale.pop_back(); };
    // (1) the check: a texture whose capture an earlier picture left out, read before this picture writes it
    for (size_t i = 0; i < n && !g_sfeStale.empty(); ++i) {
        const Cmd& x = cmds[i];
        if (x.type == CMD_DRAW) {
            const DrawRec& d = g_buf.draws[x.arg];
            for (int m = 0; m < 8; ++m) {
                if (!(d.texMask & (1u << m))) continue;
                const int k = staleAt(d.tex[m].texId);
                if (k < 0) continue;
                if (g_sfeCtr.stale + g_sfeTotal.stale < 8 && port_log_summary)
                    port_log_summary("shadowfetch: STALE READ: picture %llu, draw %u samples texture %u before it is captured again",
                                     static_cast<unsigned long long>(g_frame), static_cast<unsigned>(i), d.tex[m].texId);
                ++g_sfeCtr.stale;
                unstale(k);
            }
        } else if (x.type == CMD_COPY_TEX) {
            const CopyTexCmd& y = g_buf.copyTex[x.arg];
            int k = y.srcTex ? staleAt(y.srcTex) : -1;
            if (k >= 0) { ++g_sfeCtr.stale; unstale(k); }
            k = staleAt(y.texId);
            if (k >= 0) unstale(k);
        } else if (x.type == CMD_TEX_FREE || x.type == CMD_TEX_UPLOAD) {
            const int k = staleAt(x.type == CMD_TEX_FREE ? x.arg : g_buf.uploads[x.arg].texId);
            if (k >= 0) unstale(k);
        }
    }
    // (2) this picture's pairs
    const bool usable = g_sfeUsable.load(std::memory_order_acquire) != 0;
    int efbW = g_sfeEfbW, efbH = g_sfeEfbH;
    for (size_t i = 0; i < n; ++i) {
        if (cmds[i].type == CMD_SET_TARGET) {
            efbW = g_buf.targets[cmds[i].arg].efbW;
            efbH = g_buf.targets[cmds[i].arg].efbH;
            continue;
        }
        if (cmds[i].type != CMD_COPY_TEX) continue;
        CopyTexCmd& c = g_buf.copyTex[cmds[i].arg];
        c.fetchMode = 0;
        c.fetchDraw = 0;
        if (c.skip || c.fromPass || c.passMore || c.srcTex || c.dstTexW || c.depth || c.fill || c.texId == 0) continue;
        if (c.clear.enable || c.clearElided || CopyReads(c) != CH_A) continue;
        if (c.add[0] != 0 || c.add[1] != 0 || c.add[2] != 0 || c.add[3] != 0) continue;
        if (c.src.x != 0 || c.src.y != 0 || c.src.w != efbW || c.src.h != efbH || c.dstW != c.src.w || c.dstH != c.src.h) continue;
        // (the next command that draws must be the fill; in between only what touches no picture: shaders, programs, uploads of others)
        size_t j = i + 1;
        for (; j < n; ++j) {
            const Cmd& x = cmds[j];
            if (x.type == CMD_SHADER || x.type == CMD_PROGRAM || x.type == CMD_VARIANT || x.type == CMD_BUF_UPLOAD) continue;
            if (x.type == CMD_TEX_UPLOAD && g_buf.uploads[x.arg].texId != c.texId) continue;
            break;
        }
        if (j >= n || cmds[j].type != CMD_DRAW) continue;
        const uint32_t di = cmds[j].arg;
        const DrawRec& d = g_buf.draws[di];
        int map = -1, coord = -1;
        if (!SfFillShape(d, map, coord) || d.tex[map].texId != c.texId || !SfeFillState(d, map)) continue;
        float rect[4];
        uint64_t vhash = 0;
        if (!SfeFillGeom(d, coord, efbW, efbH, rect, vhash)) continue;
        // (no other reader of the texture until it is written again)
        bool other = false;
        for (size_t q = j + 1; q < n && !other; ++q) {
            const Cmd& x = cmds[q];
            if (x.type == CMD_DRAW) {
                const DrawRec& e = g_buf.draws[x.arg];
                for (int m = 0; m < 8 && !other; ++m) other = (e.texMask & (1u << m)) && e.tex[m].texId == c.texId;
            } else if (x.type == CMD_COPY_TEX) {
                const CopyTexCmd& y = g_buf.copyTex[x.arg];
                if (y.srcTex == c.texId) other = true;
                else if (y.texId == c.texId && !y.skip) break;
            } else if ((x.type == CMD_TEX_FREE && x.arg == c.texId) || (x.type == CMD_TEX_UPLOAD && g_buf.uploads[x.arg].texId == c.texId)) {
                break;
            }
        }
        if (other) continue;
        ++g_sfeCtr.pairs;
        c.fetchDraw = di + 1;
        c.fetchKey = SfeKey(d, c, vhash);
        std::memcpy(c.fetchRect, rect, sizeof rect);
        const int st = SfeState(c.fetchKey);
        if (mode == 1 && usable && st == 1) {
            c.fetchMode = 2;
            c.skip = true;
            ++g_sfeCtr.fetched;
            if (staleAt(c.texId) < 0 && g_sfeStale.size() < 16) g_sfeStale.push_back(c.texId);
        } else if (usable && st == 0) {
            c.fetchMode = 1;
            ++g_sfeCtr.requests;
        }
    }
    g_sfeEfbW = efbW;
    g_sfeEfbH = efbH;
    SfeLog(mode);
}
// ================================================================ silhouette fill fusion (GXV_SILH_FUSE, silhfuse26; the pure parts: vgx_silhfuse.h)
// With GXV_EFBSWAP2 the alpha capture of MR::fillSilhouetteColor is a hand-over: the EFB's old colour buffer becomes the capture's texture, the EFB goes on with a new buffer, and the
// back end begins that scene with a draw-back quad (the old pixels, all four channels, nearest). The very next draw is the fill: one whole-EFB quad that samples that texture at the
// pixel's own place, tests TEXA > K, and blends black at alpha 0x80 (alpha channel 0) over what the draw-back wrote. This pass marks the pair when the fill's result is a function of
// the texel and the fill's own values only, and gives the fill a FUSED key (FsKey::silhFuse): the back end then draws that one quad, unblended, in place of both. Everything that
// can make the pair differ from what the two passes write is refused and counted: no swap, a capture that clears, something between capture and fill, a fill that is not the
// game's (shape, key variants, bounded or scissored by GXV_SHADOWFILL_BOUNDS / SHRINK, depth or stencil work), a blend that reads the destination or rounds twice, an alpha mode
// outside keep / zero / source. DrawRec::fsId and rs stay the ordinary fill's (the back end draws them when the swap did not happen or the fused program is not ready).
//   GXV_SILH_FUSE=0 (default) off; 1 on; 2 verify: the front end marks as for 1, the back end alternates fused / unfused pictures in blocks of GXV_SILH_FUSE_BLOCK (default 30) and
//   logs the finished pictures' difference ("silhfuse: verify cycle"). Stage-settable (silh_fuse). "silhfuse:" census line every GXV_SILH_FUSE_LOG (600) pictures.
int SilhFuseMode() { return static_cast<int>(PORT_STAGE_KNOB(int32_t, std::clamp<long>(port::stageknob::Int("GXV_SILH_FUSE", 0), 0, 2))); }
struct SzCount { uint64_t pictures = 0, captures = 0, fused = 0, why[silhfuse::Census::W_COUNT] = {}, srcZero = 0; } g_szCtr, g_szTotal;
// The fill's source colour is exactly 0 for every fragment: its one stage's colour is `d` alone (a = b = ZERO, no bias, any scale), with d ZERO or a TEV register whose rgb is 0.
bool SzColourZero(const DrawRec& d, const FsKey& k) {
    const uint32_t c = k.stage[0].color;
    const uint32_t sd = bits(c, 0, 4), sb = bits(c, 8, 4), sa = bits(c, 12, 4), bias = bits(c, 16, 2);
    if (bias != 0 || sa != 15 || sb != 15 || k.regFromVtx) return false;
    if (sd == 15) return true;
    if (sd != 0 && sd != 2 && sd != 4 && sd != 6) return false; // (PREV, C0, C1, C2 as colour)
    const uint32_t tb = d.ublock[UB_TEV];
    const size_t at = static_cast<size_t>(tb) + TEV_REG + static_cast<size_t>(sd / 2) * 4;
    if (tb == kNoBlock || at + 3 > g_buf.uni.size()) return false;
    return g_buf.uni[at] == 0.0f && g_buf.uni[at + 1] == 0.0f && g_buf.uni[at + 2] == 0.0f;
}
void SzLog(int mode) {
    static const uint64_t every = static_cast<uint64_t>(std::clamp<long>(EnvInt("GXV_SILH_FUSE_LOG", 600), 0, 1000000));
    if (!every || g_szCtr.pictures < every) return;
    const SzCount& c = g_szCtr;
    char line[900];
    int n = std::snprintf(line, sizeof line, "silhfuse: %llu pictures (GXV_SILH_FUSE=%d), %llu alpha captures, fused %llu (%.2f a picture; source colour proven 0 in %llu); refused:",
                          static_cast<unsigned long long>(c.pictures), mode, static_cast<unsigned long long>(c.captures), static_cast<unsigned long long>(c.fused),
                          c.pictures ? static_cast<double>(c.fused) / static_cast<double>(c.pictures) : 0.0, static_cast<unsigned long long>(c.srcZero));
    for (int w = 1; w < silhfuse::Census::W_COUNT && n > 0 && n < static_cast<int>(sizeof line) - 80; ++w)
        if (c.why[w]) n += std::snprintf(line + n, sizeof line - static_cast<size_t>(n), " %s %llu;", silhfuse::Census::WhyName(w), static_cast<unsigned long long>(c.why[w]));
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    g_szTotal.pictures += c.pictures; g_szTotal.captures += c.captures; g_szTotal.fused += c.fused;
    for (int w = 0; w < silhfuse::Census::W_COUNT; ++w) g_szTotal.why[w] += c.why[w];
    g_szCtr = SzCount{};
}
void SilhFusePass() {
    const int mode = SilhFuseMode();
    if (!mode) return;
    using silhfuse::Census;
    ++g_szCtr.pictures;
    std::vector<Cmd>& cmds = g_buf.cmds;
    const size_t n = cmds.size();
    int efbW = g.efbW, efbH = g.efbH;
    for (size_t i = 0; i < n; ++i) {
        if (cmds[i].type == CMD_SET_TARGET) {
            efbW = g_buf.targets[cmds[i].arg].efbW;
            efbH = g_buf.targets[cmds[i].arg].efbH;
            continue;
        }
        if (cmds[i].type != CMD_COPY_TEX) continue;
        CopyTexCmd& c = g_buf.copyTex[cmds[i].arg];
        c.fuse = 0;
        c.fuseDraw = 0;
        // (the capture: the silhouette fill's whole-EFB 1:1 alpha copy that still exists; one the stencil, the gate or the bounds left out is not a pair)
        if (c.skip || c.fromPass || c.passMore || c.srcTex || c.dstTexW || c.depth || c.fill || c.texId == 0) continue;
        if (CopyReads(c) != CH_A || c.add[0] != 0 || c.add[1] != 0 || c.add[2] != 0 || c.add[3] != 0) continue;
        if (c.src.x != 0 || c.src.y != 0 || c.src.w != efbW || c.src.h != efbH || c.dstW != c.src.w || c.dstH != c.src.h) continue;
        ++g_szCtr.captures;
        auto refuse = [&](int w) { ++g_szCtr.why[w]; };
        if (ShadowFetchMode()) { refuse(Census::W_FETCH); continue; }
        if (c.clear.enable || c.clearElided) { refuse(Census::W_COPYSHAPE); continue; }
        if (!c.swapOk) { refuse(Census::W_SWAP); continue; }
        const long j = silhfuse::NextDraw(static_cast<long>(i), static_cast<long>(n), [&](long q) -> uint8_t {
            const Cmd& x = cmds[static_cast<size_t>(q)];
            if (x.type == CMD_SHADER || x.type == CMD_PROGRAM || x.type == CMD_VARIANT || x.type == CMD_BUF_UPLOAD) return silhfuse::C_HARMLESS;
            if (x.type == CMD_TEX_UPLOAD && g_buf.uploads[x.arg].texId != c.texId) return silhfuse::C_HARMLESS;
            return x.type == CMD_DRAW ? silhfuse::C_DRAW : silhfuse::C_OTHER;
        });
        if (j < 0) { refuse(Census::W_NOFILL); continue; }
        const uint32_t di = cmds[static_cast<size_t>(j)].arg;
        DrawRec& d = g_buf.draws[di];
        int map = -1, coord = -1;
        if (!SfFillShape(d, map, coord) || d.tex[map].texId != c.texId) { refuse(Census::W_FILLSHAPE); continue; }
        if (!SfeFillState(d, map)) { refuse(Census::W_STATE); continue; }
        float rect[4];
        uint64_t vhash = 0;
        const RenderState& rs = d.rs;
        if (!SfeFillGeom(d, coord, efbW, efbH, rect, vhash) || rs.scX != 0 || rs.scY != 0 || rs.scW != efbW || rs.scH != efbH) { refuse(Census::W_GEOM); continue; }
        const FsKey& k = g_fsReg.keys[d.fsId];
        const FsKey::Stage& st = k.stage[0];
        if (k.silhFuse() || k.rejectZero() || k.atPass() || k.alphaDead() || k.preAlpha() || !FsHasAlphaTestText(k) || st.texSwap != 0xE4 || st.lodBias || st.texOff || st.ind ||
            !st.texEnable || (k.texProjMask & (1u << (st.texCoord & 7)))) { refuse(Census::W_KEY); continue; }
        if (rs.cull != CULL_NONE || d.stOp || d.occ || d.clearQ || d.hud || (rs.depthTest && (rs.depthWrite || rs.depthFunc != CMP_ALWAYS))) { refuse(Census::W_STATE); continue; }
        silhfuse::Pair pr;
        pr.blend = rs.blend; pr.eq = rs.blendEq; pr.sf = rs.blendSrc; pr.df = rs.blendDst;
        pr.colorWrite = rs.colorWrite; pr.alphaWrite = rs.alphaWrite; pr.dstAlphaZero = rs.dstAlphaZero;
        pr.srcZero = SzColourZero(d, k);
        uint8_t code = 0, am = 0;
        const int why = silhfuse::Classify(pr, code, am);
        if (why != silhfuse::RJ_OK) { refuse(why == silhfuse::RJ_TWO_ROUNDINGS && !pr.srcZero ? static_cast<int>(Census::W_SRCNOTZERO) : why); continue; }
        FsKey v = k; // (a copy: Lookup may grow the registry)
        v.setSilhFuse(code, am);
        const uint16_t vid = Lookup(g_fsReg, v, true, GenerateFragmentShader);
        d.fuseFs = static_cast<uint16_t>(vid + 1);
        c.fuse = 1;
        c.fuseDraw = di + 1;
        ++g_szCtr.fused;
        if (pr.srcZero) ++g_szCtr.srcZero;
        {
            // (the shader list / pack: the fused program and its unblended variant are learnt now, so that a later run compiles them in its first frame)
            RenderState fused = rs;
            fused.blend = 0; fused.blendSrc = BF_ONE; fused.blendDst = BF_ZERO; fused.blendEq = BE_ADD; fused.dstAlphaZero = 0;
            fused.depthTest = 0; fused.depthWrite = 0; fused.colorWrite = 1; fused.alphaWrite = 1;
            split::DrawStateRef ref{d.vsId, vid, d.texMask, 0u, &fused, d.tex};
            split::NoteVariant(ref, d.layoutMask);
        }
    }
    SzLog(mode);
}
// ================================================================ shadow stencil (GXV_SHADOW_STENCIL, pass24)
// The silhouette / shadow pass of a picture (MR::fillSilhouetteColor) is: the shadow volumes and the player's silhouette write their alpha
// into the EFB (volume back faces add a, front faces subtract a, the silhouette sets X, a later alpha 0 write clears), ScreenAlphaCapture
// copies the whole EFB's alpha (a copy scene of its own: the EFB's scene is split around it, ~0.6 ms of vertex / TA time and ~0.4 ms of
// pixel time, with a draw-back quad), and the fill quad darkens (alpha 0x80 over black) wherever the captured alpha is >= T (T = 2 in the
// game: its alpha test compares TEXA > KONST (1/255) + 0.25/255). Nothing else reads that capture.
// Here the same predicate is kept in the stencil buffer: INCR on the back-face draws, DECR on the front-face draws (alpha += a, -= a),
// REPLACE 0 where an alpha 0 is written after the first volume (ST_ZERO) and on the clear quad that begins the window, REPLACE 0x80 where
// the silhouette writes X >= T (ST_SET); the fill is the same draw with its texture replaced by the 1x1 white texture and the test
// "stencil != 0" (its own alpha test leaves out every pixel the captured alpha would have). No capture, no EFB split.
// Equal wherever alpha >= T  <=>  stencil != 0, which holds when (checked per picture, else the picture is left as it is):
//  - the window begins with a quad that writes alpha 0 over the whole EFB (the game's clear quad), the stencil is 0 there: ST_ZERO;
//  - every alpha writer after it is a volume (constant alpha a >= T, ONE / ONE, ADD or REVSUB), a MARK (a constant X >= T written over what was there,
//    no blend: the silhouette; since stencil43 by the TEV's alpha or by GXSetDstAlpha's constant, which is how the game draws it: J3DModelX display
//    list 3, 0x90) or an alpha 0 write; a volume after a mark only while it cannot take a marked pixel under T (alpha >= X - a n and stencil >=
//    0x80 - n after n subtract draws: X - 2 a n >= T and 2 n <= 127, else "volume after silhouette"); no copy, target or pass in the window;
//  - volume draws: count x a <= 255 (alpha saturates at 255, the stencil at 255 counts: equal below it; both clamp at 0);
//  - the capture is the whole-EFB 1:1 alpha copy and the fill the draw after it, the only reader of its texture; a single-sample EFB.
// A convex volume gives at most one back fragment per pixel per draw; a concave one more, which the count x a bound does not see
// (residual, see REPORT). Counters by reason: ShadowStencilCounters.
enum SsReason : int {
    SS_TAKEN = 0, SS_NOLAYERS, SS_UNUSABLE, SS_UNCLASSIFIED, SS_VOLAFTERSIL, SS_ALPHAVAL, SS_LAYERS, SS_OTHERREADER, SS_BREAK, SS_NOSTART,
    SS_ALPHATEST, SS_FILLSHAPE, SS_COPYSHAPE, SS_PASS, SS_NOCAPTURE, SS_COUNT
};
const char* const kSsName[SS_COUNT] = {"taken", "no volume or silhouette", "back end cannot (MSAA / no stencil)", "unclassified alpha writer", "volume after silhouette",
                                       "alpha value", "too many layers", "other reader", "copy/target/pass in the window", "no clearing quad", "alpha-tested reset",
                                       "fill shape", "capture shape", "isolated into a pass", "no capture"};
struct SsCount {
    uint64_t pictures = 0, windows = 0, why[SS_COUNT] = {}, stale = 0, annotated = 0;
} g_ssCtr;
// ss24: GXV_SS_TILEBOUND (0 off = c51: the layer bounds read the window's TOTAL of volume back-face draws; 1 on: they read the most back-face volumes that can cover ONE pixel, from a 16 x 12
// screen-tile grid of the draws' pixel bounds; 2 worked out and counted, nothing changes). Read once. The counters below are the verify lines of the stencil summary.
constexpr int kSsTX = 16, kSsTY = 12;
struct SsTileCtr {
    uint64_t windows = 0;        // windows that reached the layer test with every other test passed
    uint64_t oldRefused = 0;     // ... the c51 rule (total of back-face draws) would refuse
    uint64_t newRefused = 0;     // ... the tile rule refuses
    uint64_t rescued = 0;        // old refuses, tile accepts (taken only with =1)
    uint64_t tighter = 0;        // old accepts, tile refuses (merged records count more than once)
    uint64_t boxless = 0;        // windows with a back-face draw that had no pixel bounds (its scissor stood in)
    uint64_t joined = 0;         // windows with a back-face record that is a merge of several draws
    uint64_t maxOverlap = 0, maxTotal = 0, sumOverlap = 0, sumTotal = 0;
} g_ssTile;
// What the fill's refusal was (ss24; no decision uses it): 0 shape of the draw / its texture, 1 state: fog only, 2 state: other, 3 threshold, 4 geometry, 5 no draw after the capture.
uint64_t g_ssFillWhy[6] = {};
// (stencil43) the census of GXV_SHADOW_STENCIL=3 ("stencil classes:" line every 600 pictures; kept for any mode >= 1, printed only by 3): draws by class, windows by what
// they hold, which windows were taken (planned) and what made it possible, and the silhouette gate's part.
struct SsClasses {
    uint64_t pictures = 0, windows = 0;
    uint64_t drawVolBack = 0, drawVolFront = 0, drawMarkTev = 0, drawMarkDst = 0, drawZero = 0, drawUnclassified = 0, drawSilShape = 0;
    uint64_t winVolOnly = 0, winMarkOnly = 0, winBoth = 0, winNone = 0;
    uint64_t taken = 0, takenOld = 0, takenByDst = 0, takenByAfter = 0, takenByDrop = 0;
    uint64_t silDropped = 0, gatedElsewhere = 0, afterMarkWindows = 0, refusedBound = 0, refusedThreshold = 0;
    uint64_t why[SS_COUNT] = {};
} g_ssCls;
std::vector<uint32_t> g_sgGated; // (stencil43) captures the silhouette gate left out this picture (its pass runs first when both switches are on)
bool SsGated(uint32_t arg) {
    for (uint32_t a : g_sgGated)
        if (a == arg) return true;
    return false;
}
std::atomic<int> g_ssUsable{0};
std::vector<uint32_t> g_ssStale; // capture textures left out: nothing else may read them before they are written again
struct SsWin {
    uint32_t copyArg = 0, fillDraw = 0, fillTex = 0, firstDraw = 0;
    int map = 0;
    std::vector<uint32_t> draws;
};
std::vector<SsWin> g_ssWins;
int g_ssEfbW = 0, g_ssEfbH = 0;
inline float SsUni(const DrawRec& d, uint32_t off, bool& ok) {
    const uint32_t tb = d.ublock[UB_TEV];
    if (tb == kNoBlock || static_cast<size_t>(tb) + off >= g_buf.uni.size()) { ok = false; return 0.0f; }
    return g_buf.uni[tb + off];
}
// A draw whose fragment alpha is the constant REG0 alpha (and nothing else): its value in 1/255 steps, 0 when it is not one.
uint64_t g_ssFogTaken = 0; // (ss24: classifications of a key with fog that c51 refused)
// ss24: GXV_SS_FOGALPHA (default 1 = exact bug fix; 0 = the c51 behaviour): GX fog blends only the RGB of the TEV result (vgx_shadergen.cpp: `fin.rgb = mix(fin.rgb, fog colour, fg)`, no
// line touches fin.a), so a key with fog has the same fragment alpha as without it. The game's ShadowVolumeDrawer does not turn the stage's fog off (Dreadnought).
bool SsFogAlphaOk() {
    static const bool on = EnvInt("GXV_SS_FOGALPHA", 1) != 0;
    return on;
}
// g30: GXV_SS_ATRESET (default 0; 2 = count-only census, 1 = apply): allow kind == 1 (constant alpha 0) reset draws
// with alphaTestOn inside a shadow-stencil window whenever the shader either has no discard (!FsHasAlphaTestText) or
// uses a real GL `discard` (!k.rejectZero()), never `rejectZero` (which writes stencil 0 on rejected pixels).
// GXV_SS_FILLFOG (default 0; 1 = apply, exact): allow fogged shadow fill quads (fog blends fin.rgb after alpha test).
int SsAtResetMode() {
    static const int m = static_cast<int>(std::clamp<long>(EnvInt("GXV_SS_ATRESET", 0), 0, 2));
    return m;
}
bool SsFillFogOk() {
    static const bool on = EnvInt("GXV_SS_FILLFOG", 0) != 0;
    return on;
}
struct SsRescueCtr {
    uint64_t pictures = 0, draws = 0, alwaysPass = 0, realDiscard = 0, rejectZero = 0;
    uint64_t winRescued = 0, winWouldRescue = 0, fillFogTaken = 0;
} g_ssRescue;
void SsRescueLog() {
    if (SsAtResetMode() == 0 && !SsFillFogOk()) return;
    char line[320];
    std::snprintf(line, sizeof line,
                  "ss-rescue: GXV_SS_ATRESET=%d draws=%llu [alwaysPass=%llu realDiscard=%llu rejectZero=%llu] winRescued=%llu wouldRescue=%llu; GXV_SS_FILLFOG=%d fillFogTaken=%llu",
                  SsAtResetMode(), static_cast<unsigned long long>(g_ssRescue.draws),
                  static_cast<unsigned long long>(g_ssRescue.alwaysPass), static_cast<unsigned long long>(g_ssRescue.realDiscard),
                  static_cast<unsigned long long>(g_ssRescue.rejectZero), static_cast<unsigned long long>(g_ssRescue.winRescued),
                  static_cast<unsigned long long>(g_ssRescue.winWouldRescue), SsFillFogOk() ? 1 : 0,
                  static_cast<unsigned long long>(g_ssRescue.fillFogTaken));
    if (port_log_summary) port_log_summary("%s", line);
    else std::fprintf(stderr, "[vgx] %s\n", line);
    g_ssRescue = SsRescueCtr{};
}
// `why` (ss24, the culprit log): the first test that failed, 0 = none: 1 no key, 2 stage count, 3 indirect, 4 alpha test, 5 destination alpha, 6 fog (GXV_SS_FOGALPHA=0), 7 z texture,
// 8 logic op, 9 alpha is not the plain REG0 constant, 10 the constant is not a whole 1/255 step (or not known), SsClassify adds 11 no alpha write, 12 blend not ONE/ONE ADD or REVSUB,
// 13 destination alpha forced, 14 alpha write over destination alpha (no constant on the draw).
int SsConstAlpha(const DrawRec& d, int* why = nullptr) {
    int w0 = 0;
    int& why_ = why ? *why : w0;
    why_ = 0;
    if (d.fsId >= g_fsReg.keys.size()) { why_ = 1; return 0; }
    const FsKey& k = g_fsReg.keys[d.fsId];
    if (k.numStages != 1) { why_ = 2; return 0; }
    if (k.numIndStages != 0) { why_ = 3; return 0; }
    if (k.alphaTestOn) { why_ = 4; return 0; }
    if (k.dstAlpha) { why_ = 5; return 0; }
    if (k.fog && !SsFogAlphaOk()) { why_ = 6; return 0; }
    if (k.ztex) { why_ = 7; return 0; }
    if (k.logicSrc) { why_ = 8; return 0; }
    if (k.fog) ++g_ssFogTaken; // (ss24: a key c51 refused; counted per classification)
    const FsKey::Stage& st = k.stage[0];
    const uint32_t a = st.alpha;
    const uint32_t sd = bits(a, 4, 3), sc = bits(a, 7, 3), sb = bits(a, 10, 3), sa = bits(a, 13, 3);
    if (bits(a, 16, 2) != 0 || bits(a, 18, 1) != 0 || bits(a, 20, 2) != 0 || bits(a, 22, 2) != 0) { why_ = 9; return 0; } // (bias, op, scale, destination)
    const bool viaA = sa == 1 && sb == 7 && sc == 7 && sd == 7;                                              // a = A0, the rest ZERO: mix(a, b, 0)
    const bool viaD = sd == 1 && sa == 7 && sb == 7 && sc == 7;                                              // d = A0
    if (!viaA && !viaD) { why_ = 9; return 0; }
    if (k.regFromVtx & 1) { // (FoldRegs: a CPU-transformed draw carries REG0 in its vertices' colour 0, the same in every vertex)
        if (d.vbuf || !(d.layoutMask & VL_COL0) || d.vtxCount == 0) { why_ = 10; return 0; }
        const VtxLayout L = MakeLayout(d.layoutMask);
        size_t base = d.vtxOffset + static_cast<size_t>(d.vtxFirst) * L.stride;
        if (!g_buf.vtxChunks.empty()) {
            if (d.vchunk >= g_buf.vtxChunks.size()) { why_ = 10; return 0; }
            base += g_buf.vtxChunks[d.vchunk];
        }
        if (base + static_cast<size_t>(d.vtxCount) * L.stride > g_buf.vtx.size()) { why_ = 10; return 0; }
        const uint8_t a0 = g_buf.vtx[base + L.offCol0 + 3], a1 = g_buf.vtx[base + static_cast<size_t>(d.vtxCount - 1) * L.stride + L.offCol0 + 3];
        if (a0 != a1 || a0 == 0) { why_ = 10; return 0; }
        return a0;
    }
    bool ok = true;
    const float v = SsUni(d, TEV_REG + 7, ok);
    if (!ok || !(v > 0.0f) || v > 1.0f) { why_ = 10; return 0; }
    const float x = v * 255.0f;
    const int n = static_cast<int>(x + 0.5f);
    if (std::fabs(x - static_cast<float>(n)) > 1e-3f || n < 1) { why_ = 10; return 0; }
    return n;
}
// The threshold T of the fill: the least captured alpha (1/255 steps) that makes it write (TEXA > K + 0.25/255); 0: not known.
int SsFillThreshold(const DrawRec& d, int map) {
    const FsKey& k = g_fsReg.keys[d.fsId];
    const FsKey::Stage& st = k.stage[0];
    const uint32_t sb = bits(st.alpha, 10, 3);
    if (sb == 7) return 1;
    if (sb != 6) return 0;
    const uint32_t sel = st.kasel;
    float ka;
    if (sel < 8) ka = (8 - sel) / 8.0f;
    else if (sel < 16) ka = 0.0f;
    else {
        bool ok = true;
        const float v = SsUni(d, TEV_KONST + (sel & 3) * 4 + ((sel - 16) >> 2), ok);
        if (!ok) return 0;
        ka = v;
    }
    (void)map;
    const float x = ka * 255.0f;
    const int n = static_cast<int>(x + 0.5f);
    if (std::fabs(x - static_cast<float>(n)) > 1e-3f || n < 0 || n > 254) return 0;
    return n + 1;
}
// The fill's colour / alpha outputs must not depend on the texture beyond its compare (SfFillShape: numStages 1, alpha test on, the alpha
// test fails at alpha 0); no fog, z texture, logic op; one texture.
bool SsFillState(const DrawRec& d, int map, bool ignoreFog = false) {
    const FsKey& k = g_fsReg.keys[d.fsId];
    if ((k.fog && !ignoreFog) || k.ztex || k.logicSrc || k.numTexGens < 1 || d.texMask != (1u << map)) return false;
    const uint32_t c = k.stage[0].color, a = k.stage[0].alpha;
    for (int s = 0; s < 4; ++s) {
        const uint32_t in = bits(c, 4 * s, 4);
        if (in >= 8 && in <= 11) return false; // (TEXC, TEXA, RASC, RASA)
    }
    const uint32_t ac = bits(a, 7, 3);
    if (ac == 4 || ac == 5) return false;
    return d.rs.alphaWrite || d.rs.colorWrite;
}
bool SsMatIsAlpha(const CopyTexCmd& c) {
    for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 4; ++col) {
            const float v = c.mat[r][col];
            if (col < 3 && v != 0.0f) return false;
            if (col == 3 && v != 0.0f && v != 1.0f) return false;
        }
    return c.mat[3][3] == 1.0f && c.add[0] == 0 && c.add[1] == 0 && c.add[2] == 0 && c.add[3] == 0;
}
// (stencil43) A draw whose written alpha is the constant of GXSetDstAlpha (FsKey::dstAlpha, the value in its TEV block) and nothing else: no blend, alpha written, no
// alpha test (so no `discard`). Wherever its depth test passes the EFB alpha becomes exactly that constant, whatever the shader's own alpha output was. This is the
// player's silhouette as the game draws it (J3DModelX display list 3, mFlags._1C: colour off, depth GEQUAL, no depth write, GXSetDstAlpha(TRUE, 0x90) = 144), which the
// TEV-alpha test of SsConstAlpha never recognised. 0: not such a draw.
int SsDstConstAlpha(const DrawRec& d) {
    if (d.fsId >= g_fsReg.keys.size()) return 0;
    const FsKey& k = g_fsReg.keys[d.fsId];
    if (!k.dstAlpha || k.alphaTestOn) return 0;
    const uint32_t tb = d.ublock[UB_TEV];
    if (tb == kNoBlock || static_cast<size_t>(tb) + TEV_MISC + 3 > g_buf.uni.size()) return 0;
    const float x = g_buf.uni[tb + TEV_MISC + 2] * 255.0f;
    const int n = static_cast<int>(x + 0.5f);
    if (std::fabs(x - static_cast<float>(n)) > 1e-3f || n < 1 || n > 255) return 0;
    return n;
}
// 0 not a layer; 1 volume back faces (alpha += a); 2 volume front faces (alpha -= a); 3 a MARK: a constant written over what was there (alpha = a). `alpha`: the
// constant (1/255). `dst` (GXV_SHADOW_STENCIL >= 1 only): also accept the constant destination alpha writer (SsDstConstAlpha); *viaDst says which kind a 3 was.
int SsClassify(const DrawRec& d, int& alpha, bool dst = false, int* viaDst = nullptr) {
    const RenderState& rs = d.rs;
    if (viaDst) *viaDst = 0;
    alpha = SsConstAlpha(d);
    if (!alpha && dst && !rs.blend && rs.alphaWrite && rs.dstAlphaZero == 0) {
        alpha = SsDstConstAlpha(d);
        if (alpha) {
            if (viaDst) *viaDst = 1;
            return 3;
        }
    }
    if (!alpha || !rs.alphaWrite) return 0;
    if (rs.blend) {
        if (rs.blendSrc != BF_ONE || rs.blendDst != BF_ONE || rs.dstAlphaZero != 0) return 0;
        if (rs.blendEq == BE_ADD) return 1;
        if (rs.blendEq == BE_REVSUB) return 2;
        return 0;
    }
    return rs.dstAlphaZero == 0 ? 3 : 0;
}
// (dread25) Which draws made a window "unclassified alpha writer": the census behind the log's second line (GXV_SHADOW_STENCIL=1 or 2). A draw
// the window's rules cannot place, counted by (fragment shader id, blend, depth, alpha test, whether the window's volumes had begun), at most 12
// kinds; the draws' indices and the constant destination alpha (-1: the shader does not write one) are kept. Read and cleared by the log.
constexpr int kSsCulMax = 12;
struct SsCul {
    uint32_t fs = 0, n = 0, idx = 0;
    uint8_t blend = 0, src = 0, dst = 0, eq = 0, dza = 0, alphaTest = 0, dstAlpha = 0, stages = 0, colorWrite = 0, depthTest = 0, depthFunc = 0, depthWrite = 0, afterLayers = 0;
    int16_t zAlpha = -1;
    uint8_t cause = 0; // (ss24: SsConstAlpha's why, or 11..14: the state's)
};
SsCul g_ssCul[kSsCulMax];
int g_ssCulN = 0;
// ss24: one back-face volume record into the tile grid: its pixel bounds (the alpha bounds when the front end worked them out, else the scissor; both are supersets of the pixels it
// can write; an empty or missing scissor = the whole EFB) cut to the EFB, every tile they touch gets the record's weight (1 + the draws merged into it: each of them is a volume).
void SsTileAdd(const DrawRec& d, int efbW, int efbH, uint16_t* tb, bool& boxless, bool& joined) {
    const int W = efbW > 0 ? efbW : 640, H = efbH > 0 ? efbH : 528;
    int x0 = 0, y0 = 0, x1 = W, y1 = H;
    if (d.exW > 0 && d.exH > 0) { x0 = d.exX; y0 = d.exY; x1 = d.exX + d.exW; y1 = d.exY + d.exH; }
    if (d.ab && d.abW > 0 && d.abH > 0) {
        x0 = std::max<int>(x0, d.abX); y0 = std::max<int>(y0, d.abY);
        x1 = std::min<int>(x1, d.abX + d.abW); y1 = std::min<int>(y1, d.abY + d.abH);
    } else {
        boxless = true;
    }
    x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, W); y1 = std::min(y1, H);
    if (x1 <= x0 || y1 <= y0) return; // (no pixel of the EFB)
    const int tw = (W + kSsTX - 1) / kSsTX, th = (H + kSsTY - 1) / kSsTY;
    const int tx0 = x0 / tw, tx1 = std::min((x1 - 1) / tw, kSsTX - 1), ty0 = y0 / th, ty1 = std::min((y1 - 1) / th, kSsTY - 1);
    const unsigned w = 1u + d.joined;
    if (d.joined) joined = true;
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx) {
            const unsigned v = tb[ty * kSsTX + tx] + w;
            tb[ty * kSsTX + tx] = static_cast<uint16_t>(v > 60000u ? 60000u : v);
        }
}
const char* const kSsCause[15] = {"constant ok", "no key", "TEV stage count", "indirect stage", "alpha test", "destination alpha", "fog", "z texture", "logic op", "alpha is not plain REG0",
                                  "constant not a whole step / unknown", "no alpha write", "blend not ONE/ONE add or reverse subtract", "destination alpha forced", "other"};
uint8_t SsCulpritCause(const DrawRec& d) {
    int why = 0;
    const int a = SsConstAlpha(d, &why);
    if (why) return static_cast<uint8_t>(why);
    if (!a) return 10;
    const RenderState& rs = d.rs;
    if (!rs.alphaWrite) return 11;
    if (rs.blend && (rs.blendSrc != BF_ONE || rs.blendDst != BF_ONE || (rs.blendEq != BE_ADD && rs.blendEq != BE_REVSUB))) return 12;
    if (rs.dstAlphaZero != 0) return 13;
    return 14;
}
void SsNoteCulprit(const DrawRec& d, bool afterLayers) {
    const RenderState& rs = d.rs;
    const FsKey* k = d.fsId < g_fsReg.keys.size() ? &g_fsReg.keys[d.fsId] : nullptr;
    int zAlpha = -1;
    if (k && k->dstAlpha) {
        const uint32_t tb = d.ublock[UB_TEV];
        if (tb != kNoBlock && static_cast<size_t>(tb) + TEV_MISC + 3 <= g_buf.uni.size()) zAlpha = static_cast<int>(g_buf.uni[tb + TEV_MISC + 2] * 255.0f + 0.5f);
    }
    for (int i = 0; i < g_ssCulN; ++i) {
        SsCul& c = g_ssCul[i];
        if (c.fs == d.fsId && c.blend == (rs.blend ? 1 : 0) && c.src == rs.blendSrc && c.dst == rs.blendDst && c.eq == rs.blendEq && c.dza == rs.dstAlphaZero &&
            c.depthTest == (rs.depthTest ? 1 : 0) && c.depthFunc == (rs.depthFunc & 7) && c.depthWrite == (rs.depthWrite ? 1 : 0) && c.afterLayers == (afterLayers ? 1 : 0) &&
            c.zAlpha == zAlpha) {
            ++c.n;
            c.idx += d.idxCount;
            return;
        }
    }
    if (g_ssCulN >= kSsCulMax) return;
    SsCul& c = g_ssCul[g_ssCulN++];
    c.fs = d.fsId;
    c.cause = SsCulpritCause(d);
    c.n = 1;
    c.idx = d.idxCount;
    c.blend = rs.blend ? 1 : 0;
    c.src = static_cast<uint8_t>(rs.blendSrc);
    c.dst = static_cast<uint8_t>(rs.blendDst);
    c.eq = static_cast<uint8_t>(rs.blendEq);
    c.dza = static_cast<uint8_t>(rs.dstAlphaZero);
    c.alphaTest = k ? k->alphaTestOn : 0;
    c.dstAlpha = k ? k->dstAlpha : 0;
    c.stages = k ? k->numStages : 0;
    c.colorWrite = rs.colorWrite ? 1 : 0;
    c.depthTest = rs.depthTest ? 1 : 0;
    c.depthFunc = static_cast<uint8_t>(rs.depthFunc & 7);
    c.depthWrite = rs.depthWrite ? 1 : 0;
    c.afterLayers = afterLayers ? 1 : 0;
    c.zAlpha = static_cast<int16_t>(zAlpha);
}
// (shadow25 / stencil43) the shape of the player's silhouette whatever its alpha is made of: alpha written, no blend, depth GEQUAL without a depth write.
inline bool SgSilhouetteShape(const DrawRec& d) {
    const RenderState& rs = d.rs;
    return !rs.blend && rs.alphaWrite && rs.depthTest && (rs.depthFunc & 7) == CMP_GEQUAL && !rs.depthWrite;
}
// (stencil43) GXV_SHADOW_STENCIL=3: the class census, every 600 pictures. Nothing in the pictures changes in this mode (the plan of mode 2).
void SsClassesLog(int mode) {
    const SsClasses& c = g_ssCls;
    char line[1100];
    int at = std::snprintf(line, sizeof line,
                           "stencil classes: GXV_SHADOW_STENCIL=%d, %llu pictures, %llu windows (volumes only %llu, volumes + marks %llu, marks only %llu, nothing classified %llu); draws: volume back %llu, "
                           "volume front %llu, alpha-0 reset %llu, mark by TEV constant %llu, mark by constant destination alpha (the silhouette) %llu, silhouette-shaped unplaced %llu, unclassified %llu; "
                           "windows with a volume after a mark %llu; the path would now be taken in %llu (by the old classes alone %llu; thanks to the destination-alpha class %llu, to the volume-after-mark rule %llu, "
                           "to dropping a silhouette-shaped writer %llu); left to the silhouette gate %llu; silhouette draws left out under hint 1 %llu; refused by the bound %llu, by the threshold %llu;",
                           mode, static_cast<unsigned long long>(c.pictures), static_cast<unsigned long long>(c.windows), static_cast<unsigned long long>(c.winVolOnly),
                           static_cast<unsigned long long>(c.winBoth), static_cast<unsigned long long>(c.winMarkOnly), static_cast<unsigned long long>(c.winNone),
                           static_cast<unsigned long long>(c.drawVolBack), static_cast<unsigned long long>(c.drawVolFront), static_cast<unsigned long long>(c.drawZero),
                           static_cast<unsigned long long>(c.drawMarkTev), static_cast<unsigned long long>(c.drawMarkDst), static_cast<unsigned long long>(c.drawSilShape),
                           static_cast<unsigned long long>(c.drawUnclassified), static_cast<unsigned long long>(c.afterMarkWindows), static_cast<unsigned long long>(c.taken),
                           static_cast<unsigned long long>(c.takenOld), static_cast<unsigned long long>(c.takenByDst), static_cast<unsigned long long>(c.takenByAfter),
                           static_cast<unsigned long long>(c.takenByDrop), static_cast<unsigned long long>(c.gatedElsewhere), static_cast<unsigned long long>(c.silDropped),
                           static_cast<unsigned long long>(c.refusedBound), static_cast<unsigned long long>(c.refusedThreshold));
    for (int r = 1; r < SS_COUNT; ++r)
        if (c.why[r] && at < static_cast<int>(sizeof line) - 70)
            at += std::snprintf(line + at, sizeof line - at, " %s %llu;", kSsName[r], static_cast<unsigned long long>(c.why[r]));
    if (port_log_summary) port_log_summary("%s", line);
    g_ssCls = SsClasses{};
}
bool SsFullCover(const DrawRec& d, int efbW, int efbH) {
    if (!(d.cover || d.coverCull) || d.exX > 0 || d.exY > 0 || d.exX + d.exW < efbW || d.exY + d.exH < efbH) return false;
    const RenderState& rs = d.rs;
    return !rs.depthTest || (rs.depthFunc & 7) == CMP_ALWAYS;
}
void ShadowStencilPass() {
    const int mode = ShadowStencilMode();
    g_ssZeroFill.clear();
    if (!mode) return;
    ++g_ssCtr.pictures;
    g_ssWins.clear();
    if (!g_ssEfbW) { g_ssEfbW = g.efbW; g_ssEfbH = g.efbH; }
    std::vector<Cmd>& cmds = g_buf.cmds;
    const size_t n = cmds.size();
    auto staleAt = [&](uint32_t id) -> int {
        for (size_t k = 0; k < g_ssStale.size(); ++k)
            if (g_ssStale[k] == id) return static_cast<int>(k);
        return -1;
    };
    // (1) the check: a capture texture an earlier picture left out, read before this picture writes it
    for (size_t i = 0; i < n && !g_ssStale.empty(); ++i) {
        const Cmd& x = cmds[i];
        if (x.type == CMD_DRAW) {
            const DrawRec& d = g_buf.draws[x.arg];
            for (int m = 0; m < 8; ++m) {
                if (!(d.texMask & (1u << m))) continue;
                const int k = staleAt(d.tex[m].texId);
                if (k < 0) continue;
                if (g_ssCtr.stale < 8 && port_log_summary)
                    port_log_summary("shadow-stencil: STALE READ: picture %llu, draw %u samples texture %u whose capture was left out", static_cast<unsigned long long>(g_frame),
                                     static_cast<unsigned>(i), d.tex[m].texId);
                ++g_ssCtr.stale;
                g_ssStale[k] = g_ssStale.back();
                g_ssStale.pop_back();
            }
        } else if (x.type == CMD_COPY_TEX) {
            const CopyTexCmd& y = g_buf.copyTex[x.arg];
            int k = y.srcTex ? staleAt(y.srcTex) : -1;
            if (k >= 0) { ++g_ssCtr.stale; g_ssStale[k] = g_ssStale.back(); g_ssStale.pop_back(); }
            k = staleAt(y.texId);
            if (k >= 0 && !y.skip) { g_ssStale[k] = g_ssStale.back(); g_ssStale.pop_back(); }
        } else if (x.type == CMD_TEX_FREE || x.type == CMD_TEX_UPLOAD) {
            const int k = staleAt(x.type == CMD_TEX_FREE ? x.arg : g_buf.uploads[x.arg].texId);
            if (k >= 0) { g_ssStale[k] = g_ssStale.back(); g_ssStale.pop_back(); }
        }
    }
    // (2) the windows
    const bool usable = mode >= 2 || g_ssUsable.load(std::memory_order_acquire) != 0; // (=2, 3: the plan is made as if the back end could)
    // (stencil43) the classes and rules beyond pass24's, all under GXV_SHADOW_STENCIL >= 1:
    //  - a MARK (class 3) is any constant alpha written over what was there, no blend: the TEV-alpha constant of pass24 and the constant destination alpha of the game's
    //    silhouette (SsDstConstAlpha). A mark is a pure set: where its depth test passes alpha becomes c >= T and the stencil 0x80, whatever the counters held.
    //  - a volume AFTER a mark is allowed when it cannot take the marked pixel under T: alpha >= c - a * (front-face draws after the mark) (each SUBTRACT lowers it by at
    //    most a, an ADD never lowers it, the clamp at 255 only lifts it), and the stencil >= 0x80 - (the same draws): both stay non-zero while 2 * a * n <= c - T (a concave
    //    volume may give two fronts a pixel) and 2 * n <= 127. Else "volume after silhouette" as before.
    //  - GXV_SILH_GATE = 1 as well: the silhouette's draws are dropped from a window the stencil takes when the game's hint says it cannot show (hint 1), and a
    //    silhouette-shaped writer this classification cannot place is dropped the same way instead of refusing the window (it is the gate's non-exact claim, nothing else).
    const bool gateDrop = mode == 1 && SilhGateMode() == 1;
    const bool gateAny = SilhGateMode() == 1; // (also for the plan and the census: they take what the gate left)
    const bool occKeep = SilhOccMode() != 0; // (across26, GXV_SILH_OCC: the silhouette's draws are never left out: the occlusion query counts them)
    static const int brk = static_cast<int>(EnvInt("GXV_SHADOW_STENCIL_BREAK", 0)); // (tests, front end's half: 5 the threshold is not checked for a mark, 6 no bound for a volume after a mark)
    ++g_ssCls.pictures;
    int efbW = g_ssEfbW, efbH = g_ssEfbH;
    bool in = false, layers = false, sawSil = false;
    int bad = -1, nBack = 0, aVol = 0, minA = 1000;
    int markMin = 1000, subAfter = 0, addAfter = 0, nVol = 0, nMark = 0, usedDst = 0;
    bool winAtRescued = false, winAtWould = false, winAtBlocked = false;
    const int tileMode = (mode >= 1) ? SsTileMode() : 0;
    uint16_t tileBack[kSsTX * kSsTY] = {}; // (ss24: the weighted back-face volume draws whose pixel bounds touch each tile of this window; 384 bytes)
    bool tileBoxless = false, tileJoined = false;
    uint64_t wd[7] = {}; // (this window's draws: volume back, volume front, mark TEV, mark destination alpha, zero, unclassified, silhouette-shaped)
    std::vector<size_t> silIdx, unkSil; // (command indices of the window's mark draws / its silhouette-shaped writers no class places)
    std::vector<uint8_t> dropCmd;
    std::vector<std::pair<uint32_t, uint8_t>> pend;
    auto resetWin = [&]() {
        layers = false; sawSil = false; bad = -1; nBack = 0; aVol = 0; minA = 1000;
        markMin = 1000; subAfter = addAfter = nVol = nMark = usedDst = 0;
        winAtRescued = winAtWould = winAtBlocked = false;
        if (tileMode) { std::memset(tileBack, 0, sizeof tileBack); tileBoxless = tileJoined = false; }
        for (uint64_t& v : wd) v = 0;
        silIdx.clear();
        unkSil.clear();
        pend.clear();
    };
    // the census of a window that reached a verdict (a capture or a break): draws by class, what it holds
    auto noteWin = [&]() {
        ++g_ssCls.windows;
        g_ssCls.drawVolBack += wd[0]; g_ssCls.drawVolFront += wd[1]; g_ssCls.drawMarkTev += wd[2]; g_ssCls.drawMarkDst += wd[3];
        g_ssCls.drawZero += wd[4]; g_ssCls.drawUnclassified += wd[5]; g_ssCls.drawSilShape += wd[6];
        if (nVol && nMark) ++g_ssCls.winBoth;
        else if (nVol) ++g_ssCls.winVolOnly;
        else if (nMark) ++g_ssCls.winMarkOnly;
        else ++g_ssCls.winNone;
        if (nMark && (subAfter || addAfter)) ++g_ssCls.afterMarkWindows;
    };
    auto endWin = [&](int why) {
        if (in && layers) { ++g_ssCtr.windows; ++g_ssCtr.why[bad >= 0 ? bad : why]; noteWin(); ++g_ssCls.why[bad >= 0 ? bad : why]; }
        in = false;
    };
    for (size_t i = 0; i < n; ++i) {
        const Cmd c = cmds[i];
        switch (c.type) {
        case CMD_SET_TARGET:
            endWin(SS_BREAK);
            efbW = g_buf.targets[c.arg].efbW;
            efbH = g_buf.targets[c.arg].efbH;
            break;
        case CMD_COPY_DISP: case CMD_PASS_BEGIN: case CMD_PRESENT:
            endWin(SS_BREAK);
            break;
        case CMD_DRAW: {
            DrawRec& d = g_buf.draws[c.arg];
            if (d.idxCount == 0) break;
            const int kind = SfAlphaKind(d);
            if (kind == 1 && SsFullCover(d, efbW, efbH)) { // (a window begins, or begins again: the alpha is 0 everywhere, so the stencil is made 0)
                in = true;
                resetWin();
                pend.emplace_back(c.arg, static_cast<uint8_t>(ST_ZERO | ST_WINBEGIN));
                break;
            }
            if (!in || kind == 0) break;
            if (kind == 1) {
                if (!layers) break; // (alpha was 0 there already, and so is the stencil)
                if (d.fsId < g_fsReg.keys.size() && g_fsReg.keys[d.fsId].alphaTestOn) {
                    const FsKey& rk = g_fsReg.keys[d.fsId];
                    const int arm = SsAtResetMode();
                    const bool noText = !FsHasAlphaTestText(rk);
                    const bool rejZ = rk.rejectZero() != 0 || rk.silhFuse() != 0;
                    if (arm != 0) {
                        ++g_ssRescue.draws;
                        if (noText) ++g_ssRescue.alwaysPass;
                        else if (rejZ) ++g_ssRescue.rejectZero;
                        else ++g_ssRescue.realDiscard;
                    }
                    const bool rescued = (arm == 1) && (noText || !rejZ);
                    if (rescued) {
                        winAtRescued = true;
                    } else {
                        if (arm == 2 && (noText || !rejZ)) winAtWould = true;
                        else winAtBlocked = true;
                        if (bad < 0) bad = SS_ALPHATEST;
                    }
                }
                ++wd[4];
                pend.emplace_back(c.arg, static_cast<uint8_t>(ST_ZERO));
                break;
            }
            int alpha = 0, viaDst = 0;
            const int cls = SsClassify(d, alpha, true, &viaDst);
            if (!cls) {
                if (gateAny && SgSilhouetteShape(d)) { // (a silhouette-shaped writer no class places: dropped with the gate's hint, else the window is refused at its capture)
                    unkSil.push_back(i);
                    ++wd[6];
                    SsNoteCulprit(d, layers);
                    break;
                }
                if (bad < 0) bad = SS_UNCLASSIFIED;
                ++wd[5];
                SsNoteCulprit(d, layers);
                break;
            }
            layers = true;
            if (alpha < minA) minA = alpha;
            if (cls == 3) {
                sawSil = true;
                ++nMark;
                ++wd[viaDst ? 3 : 2];
                if (viaDst) usedDst = 1;
                if (alpha < markMin) markMin = alpha;
                silIdx.push_back(i);
                pend.emplace_back(c.arg, static_cast<uint8_t>(ST_SET));
                break;
            }
            ++nVol;
            if (sawSil) { if (cls == 2) ++subAfter; else ++addAfter; }
            if (aVol && aVol != alpha && bad < 0) bad = SS_ALPHAVAL;
            aVol = alpha;
            if (cls == 1) {
                ++nBack;
                if (tileMode) SsTileAdd(d, efbW, efbH, tileBack, tileBoxless, tileJoined);
            }
            ++wd[cls == 1 ? 0 : 1];
            pend.emplace_back(c.arg, static_cast<uint8_t>(cls == 1 ? ST_INCR : ST_DECR));
            break;
        }
        case CMD_COPY_TEX: {
            CopyTexCmd& t = g_buf.copyTex[c.arg];
            if (!in) break;
            if (t.skip && SsGated(c.arg)) { in = false; ++g_ssCls.gatedElsewhere; break; } // (stencil43: the silhouette gate took this window whole, capture and fill)
            if (!layers) { in = false; break; } // (a window without a volume: left to GXV_SHADOWFILL_BOUNDS)
            int why = -1;
            // the capture: whole-EFB 1:1, alpha only, nothing added, no clear
            if (t.skip || t.fromPass || t.passMore || t.srcTex || t.dstTexW || t.depth || t.fill || t.texId == 0 || t.clear.enable || t.clearElided ||
                CopyReads(t) != CH_A || !SsMatIsAlpha(t) || t.src.x != 0 || t.src.y != 0 || t.src.w != efbW || t.src.h != efbH || t.dstW != t.src.w || t.dstH != t.src.h) {
                endWin(SS_BREAK);
                break;
            }
            // the next draw is the fill
            size_t j = i + 1;
            for (; j < n; ++j) {
                const Cmd& x = cmds[j];
                if (x.type == CMD_SHADER || x.type == CMD_PROGRAM || x.type == CMD_VARIANT || x.type == CMD_BUF_UPLOAD) continue;
                if (x.type == CMD_TEX_UPLOAD && g_buf.uploads[x.arg].texId != t.texId) continue;
                break;
            }
            int map = -1, coord = -1, thr = 0;
            bool usedFillFog = false;
            DrawRec* fd = nullptr;
            if (j < n && cmds[j].type == CMD_DRAW) {
                fd = &g_buf.draws[cmds[j].arg];
                float rect[4];
                uint64_t vhash = 0;
                // (the fill reads the texel of the pixel it shades: one quad whose coordinate is (X / efbW, Y / efbH) at every vertex, over the whole EFB)
                int fw = -1;
                if (!SfFillShape(*fd, map, coord) || fd->tex[map].texId != t.texId) fw = 0;
                else if (!SsFillState(*fd, map, SsFillFogOk())) fw = SsFillState(*fd, map, true) ? 1 : 2;
                else if (!(thr = SsFillThreshold(*fd, map))) fw = 3;
                else if (!SfeFillGeom(*fd, coord, efbW, efbH, rect, vhash) || rect[0] > -1.0f + 1e-4f || rect[1] > -1.0f + 1e-4f || rect[2] < 1.0f - 1e-4f || rect[3] < 1.0f - 1e-4f) fw = 4;
                if (fw < 0 && SsFillFogOk() && !SsFillState(*fd, map, false)) usedFillFog = true;
                if (fw >= 0) { fd = nullptr; ++g_ssFillWhy[fw]; }
            } else {
                ++g_ssFillWhy[5];
            }
            if (!fd) why = SS_FILLSHAPE;
            // no other reader of the texture, before or after
            bool other = false;
            for (size_t q = 0; q < n && !other && fd; ++q) {
                if (q == j) continue;
                const Cmd& x = cmds[q];
                if (x.type == CMD_DRAW) {
                    const DrawRec& e = g_buf.draws[x.arg];
                    for (int m = 0; m < 8 && !other; ++m) other = (e.texMask & (1u << m)) && e.tex[m].texId == t.texId;
                } else if (x.type == CMD_COPY_TEX) {
                    other = g_buf.copyTex[x.arg].srcTex == t.texId;
                }
            }
            int reason = bad;
            if (reason < 0 && why >= 0) reason = why;
            if (reason < 0 && other) reason = SS_OTHERREADER;
            // (stencil43) the silhouette-shaped writers no class places: only a window whose hint says "cannot show" can leave them out (SILH_GATE=1)
            const bool dropUnk = !unkSil.empty() && gateAny && t.silhHint == 1 && !occKeep;
            if (reason < 0 && !unkSil.empty() && !dropUnk) reason = SS_UNCLASSIFIED;
            if (reason < 0 && (minA < thr || (aVol && aVol < thr)) && brk != 5) reason = SS_ALPHAVAL;
            // a volume after a mark: alpha stays >= markMin - 2 a n and the stencil >= 0x80 - 2 n (see above)
            bool usedAfter = false;
            if (reason < 0 && nMark && (subAfter || addAfter)) {
                const bool ok = brk == 6 || (aVol * 2 * subAfter + thr <= markMin && 2 * subAfter <= 127);
                if (!ok) reason = SS_VOLAFTERSIL;
                else usedAfter = true;
            }
            // The layer bounds: at a pixel the alpha is at most (a x the back-face volumes covering it) and the stencil counter at most (the same count); a silhouette is 64 of the counter's 128
            // values (dm65: port/shadow_stencil.h). c51 used the window's total of back-face draws (nBack) for "the most covering one pixel"; GXV_SS_TILEBOUND=1 uses the grid's maximum (ss24):
            // every pixel lies in one tile, every draw that can write the pixel has its bounds over that tile, so the tile's sum is at least the pixel's count.
            // NOTE the 255 rule is the one that binds with a = 4 (63 x 4 = 252): the 63 / 127 stencil limits matter only above a = 2.
            if (reason < 0) {
                const auto refuses = [&](int nb) { return (aVol && nb * aVol > 255) || nb > (nMark ? port::shstencil::kMaxLayers : port::shstencil::kCountMax); };
                int nEff = nBack;
                if (tileMode) {
                    int mx = 0;
                    for (int q = 0; q < kSsTX * kSsTY; ++q) mx = std::max<int>(mx, tileBack[q]);
                    const bool o = refuses(nBack), nw = refuses(mx);
                    ++g_ssTile.windows;
                    g_ssTile.oldRefused += o; g_ssTile.newRefused += nw; g_ssTile.rescued += (o && !nw); g_ssTile.tighter += (!o && nw);
                    g_ssTile.boxless += tileBoxless; g_ssTile.joined += tileJoined;
                    g_ssTile.maxOverlap = std::max<uint64_t>(g_ssTile.maxOverlap, static_cast<uint64_t>(mx));
                    g_ssTile.maxTotal = std::max<uint64_t>(g_ssTile.maxTotal, static_cast<uint64_t>(nBack));
                    g_ssTile.sumOverlap += static_cast<uint64_t>(mx); g_ssTile.sumTotal += static_cast<uint64_t>(nBack);
                    if (tileMode == 1) nEff = mx;
                }
                if (refuses(nEff)) reason = SS_LAYERS;
            }
            if (reason < 0 && !usable) reason = SS_UNUSABLE;
            ++g_ssCtr.windows;
            ++g_ssCtr.why[reason < 0 ? SS_TAKEN : reason];
            noteWin();
            ++g_ssCls.why[reason < 0 ? SS_TAKEN : reason];
            if (reason < 0) {
                ++g_ssCls.taken;
                if (!usedDst && !usedAfter && !dropUnk) ++g_ssCls.takenOld; // (the classes of pass24 alone would have taken it)
                if (usedDst) ++g_ssCls.takenByDst;
                if (usedAfter) ++g_ssCls.takenByAfter;
                if (dropUnk) ++g_ssCls.takenByDrop;
                if (mode == 1) {
                    if (winAtRescued) ++g_ssRescue.winRescued;
                    if (usedFillFog) ++g_ssRescue.fillFogTaken;
                }
            } else if (winAtWould && !winAtBlocked && reason == SS_ALPHATEST && why < 0 && !other && (unkSil.empty() || dropUnk) &&
                       !((minA < thr || (aVol && aVol < thr)) && brk != 5) && usable) {
                bool wouldOk = true;
                if (nMark && (subAfter || addAfter))
                    wouldOk = brk == 6 || (aVol * 2 * subAfter + thr <= markMin && 2 * subAfter <= 127);
                if (wouldOk) {
                    int nEff = nBack;
                    if (tileMode == 1) {
                        int mx = 0;
                        for (int q = 0; q < kSsTX * kSsTY; ++q) mx = std::max<int>(mx, tileBack[q]);
                        nEff = mx;
                    }
                    if ((aVol && nEff * aVol > 255) || nEff > (nMark ? port::shstencil::kMaxLayers : port::shstencil::kCountMax)) wouldOk = false;
                }
                if (wouldOk) ++g_ssRescue.winWouldRescue;
            }
            if (reason == SS_ALPHAVAL && minA < thr) ++g_ssCls.refusedThreshold;
            if (reason == SS_VOLAFTERSIL) ++g_ssCls.refusedBound;
            in = false;
            if (reason >= 0) break;
            // (fix47, GXV_CORNERPASS: "the fill's certificate" at CornerTrack) every alpha writer of the window is a classified layer and every check above was really
            // made (no test break, no unplaced silhouette-shaped writer): after this fill the EFB's alpha is 0
            if (CornerPassMode() != 0 && unkSil.empty() && brk == 0) g_ssZeroFill.push_back(cmds[j].arg);
            const bool dropSil = (gateDrop && t.silhHint == 1 && !occKeep && (!silIdx.empty() || !unkSil.empty())); // (hint 1: the silhouette cannot show: its draws are left out)
            if (dropSil || (gateAny && t.silhHint == 1 && (!silIdx.empty() || !unkSil.empty()))) g_ssCls.silDropped += silIdx.size() + unkSil.size();
            if (mode == 1) {
                if (dropSil) {
                    if (dropCmd.empty()) dropCmd.assign(n, 0);
                    for (size_t ix : silIdx) dropCmd[ix] = 1;
                    for (size_t ix : unkSil) dropCmd[ix] = 1;
                    size_t o = 0;
                    for (size_t q = 0; q < pend.size(); ++q) {
                        bool isSil = false;
                        for (size_t ix : silIdx) isSil = isSil || cmds[ix].arg == pend[q].first;
                        if (!isSil) pend[o++] = pend[q];
                    }
                    pend.resize(o);
                }
                SsWin w;
                w.copyArg = c.arg;
                w.fillDraw = cmds[j].arg;
                w.fillTex = t.texId;
                w.map = map;
                w.firstDraw = pend.front().first;
                for (const auto& pr : pend) {
                    g_buf.draws[pr.first].stOp = pr.second;
                    w.draws.push_back(pr.first);
                }
                g_ssCtr.annotated += pend.size();
                fd->stOp = ST_TEST;
                fd->tex[map].texId = 0;
                t.skip = true;
                t.stWin = 1;
                if (staleAt(t.texId) < 0 && g_ssStale.size() < 16) g_ssStale.push_back(t.texId);
                g_ssWins.push_back(std::move(w));
            }
            break;
        }
        default:
            break;
        }
    }
    endWin(SS_NOCAPTURE);
    g_ssEfbW = efbW;
    g_ssEfbH = efbH;
    if (!dropCmd.empty()) {
        size_t out = 0;
        for (size_t i = 0; i < n; ++i)
            if (!dropCmd[i]) cmds[out++] = cmds[i];
        cmds.resize(out);
    }
    g_sgGated.clear();
    static const long logEvery = EnvInt("GXV_SHADOW_STENCIL_LOG_EVERY", 600); // (the back end's line has the same knob; tests use a short window)
    if (mode == 3 && g_ssCls.pictures >= static_cast<uint64_t>(logEvery)) SsClassesLog(mode);
    if ((SsAtResetMode() != 0 || SsFillFogOk()) && ++g_ssRescue.pictures >= static_cast<uint64_t>(logEvery)) SsRescueLog();
}
// After the passes were made (IsolatePasses, AtlasPasses): a window that an offscreen pass cuts into is undone (its draws would not be on the
// EFB, whose stencil it is).
void ShadowStencilVerify() {
    if (g_ssWins.empty()) return;
    std::vector<Cmd>& cmds = g_buf.cmds;
    for (SsWin& w : g_ssWins) {
        bool open = false, bad = false;
        for (size_t i = 0; i < cmds.size() && !bad; ++i) {
            const Cmd& c = cmds[i];
            if (c.type == CMD_DRAW) {
                if (c.arg == w.firstDraw) open = true;
                else if (open && c.arg == w.fillDraw) open = false;
            } else if (open && (c.type == CMD_PASS_BEGIN || c.type == CMD_COPY_DISP || c.type == CMD_SET_TARGET || c.type == CMD_PRESENT ||
                                (c.type == CMD_COPY_TEX && c.arg != w.copyArg))) {
                bad = true;
            }
        }
        if (!bad) continue;
        for (uint32_t di : w.draws) g_buf.draws[di].stOp = 0;
        DrawRec& f = g_buf.draws[w.fillDraw];
        f.stOp = 0;
        f.tex[w.map].texId = w.fillTex;
        CopyTexCmd& t = g_buf.copyTex[w.copyArg];
        t.skip = false;
        t.stWin = 0;
        for (size_t k = 0; k < g_ssStale.size(); ++k)
            if (g_ssStale[k] == w.fillTex) { g_ssStale[k] = g_ssStale.back(); g_ssStale.pop_back(); break; }
        --g_ssCtr.why[SS_TAKEN];
        ++g_ssCtr.why[SS_PASS];
    }
    g_ssWins.clear();
}
} // namespace
void ShadowStencilUsable(int on) { g_ssUsable.store(on, std::memory_order_release); }
int ShadowStencilMode() {
    static const int m = static_cast<int>(std::clamp<long>(EnvInt("GXV_SHADOW_STENCIL", 0), 0, 3));
    return m;
}
void ShadowStencilCounters(uint64_t out[24]) {
    out[0] = g_ssCtr.pictures;
    out[1] = g_ssCtr.windows;
    out[2] = g_ssCtr.stale;
    out[3] = g_ssCtr.annotated;
    for (int r = 0; r < 15 && 4 + r < 24; ++r) out[4 + r] = g_ssCtr.why[r];
}
const char* ShadowStencilReasonName(int r) { return r >= 0 && r < 15 ? kSsName[r] : "?"; }
const char* ShadowStencilCauseName(int c) { return c >= 0 && c < 15 ? kSsCause[c] : "?"; }
// (ss24) The verify counters: [0..8] the tile bound (windows, old refuses, tile refuses, rescued, tighter, boxless, joined, max overlap, max total), [9] sum overlap, [10] sum total, [11] mode,
// [12] fog keys taken that c51 refused, [13..18] the fill's refusal by cause (g_ssFillWhy).
void ShadowStencilExtra(uint64_t out[24]) {
    out[0] = g_ssTile.windows; out[1] = g_ssTile.oldRefused; out[2] = g_ssTile.newRefused; out[3] = g_ssTile.rescued; out[4] = g_ssTile.tighter;
    out[5] = g_ssTile.boxless; out[6] = g_ssTile.joined; out[7] = g_ssTile.maxOverlap; out[8] = g_ssTile.maxTotal; out[9] = g_ssTile.sumOverlap; out[10] = g_ssTile.sumTotal;
    out[11] = static_cast<uint64_t>(SsTileMode()); out[12] = g_ssFogTaken;
    for (int i = 0; i < 6; ++i) out[13 + i] = g_ssFillWhy[i];
}
// ================================================================ silhouette gate (GXV_SILH_GATE, shadow25)
// The picture's silhouette / shadow pass is: [clear quad: alpha 0 everywhere] .. shadow volumes (alpha counted) .. the player's silhouette (alpha written
// where he is BEHIND the map: depth GEQUAL, no blend) .. whole-EFB A8 capture .. fill quad (reads the capture, darkens where alpha >= 2). The capture splits
// the EFB's scene (~0.6 ms 3D + 0.5 ms TA) and the fill is a full-screen pass (~0.8 ms), every picture, though the silhouette puts no pixel there unless he is
// hidden. When the window holds no volume, the capture can only hold the silhouette's pixels. Three cases:
//  - no alpha writer at all (the player is not drawn): the capture is all 0: left out with its fill, EXACT;
//  - the player's silhouette is drawn and the game says (PORT_GX_MARK_SILH_HINT 1, MarioActor::portSilhouetteHint) that no map geometry lies between the camera
//    and him (rays, a hold of several pictures after the last hit, never in demos / near the camera): capture and fill left out. NOT exact: a map object
//    without collision in front of him is missed (see REPORT);
//  - anything else (a volume, an alpha writer that is neither, the hint 0 / 2, a fill that is not the fill, another reader): as today.
// The window must begin with the game's covering alpha-0 quad (SsFullCover, so the clear quad is marked: DrawRec::coverCull is set while this switch is on).
// GXV_SILH_GATE=2: the same decisions are counted ("silhgate:" line every 600 pictures), nothing is changed.
namespace {
enum SgReason : int { SG_GATED_EXACT = 0, SG_GATED_HINT, SG_KEEP_HINT, SG_KEEP_VOLUME, SG_KEEP_UNKNOWN, SG_KEEP_FILL, SG_KEEP_READER, SG_KEEP_SHAPE, SG_NOSTART, SG_NOWINDOW, SG_COUNT };
struct SgCount { uint64_t pictures = 0, why[SG_COUNT] = {}; } g_sgCtr, g_sgTot;
// GXV_SILH_OCC's census (every 600 pictures with the gate's line): decisions by reason, the results that arrived, the game's hint against what the query saw
// (the matrix: "may show" pictures should mostly hit, "cannot show" ones should not), and the pictures the gate left out although the query then saw him (the misses).
void SilhOccLog(int om) {
    silhocc::History& h = SilhOccHistory();
    const silhocc::Params p = SilhOccParams();
    char line[900];
    int at = std::snprintf(line, sizeof line, "silhocc: GXV_SILH_OCC=%d (hold %d, minpx %u%s):", om, p.hold, static_cast<unsigned>(p.minPx), p.needProof ? ", proof needed" : "");
    for (int w = 0; w < silhocc::kWhyCount; ++w)
        at += std::snprintf(line + at, sizeof line - at, " %s %llu;", silhocc::WhyName(w), static_cast<unsigned long long>(h.decisions[w].load()));
    const unsigned long long res = h.results.load();
    at += std::snprintf(line + at, sizeof line - at,
        " results %llu (hits ever %u, mean %.1f samples, unmatched %llu); by the game's hint no-hit/hit: none %llu/%llu, cannot-show %llu/%llu, may-show %llu/%llu;"
        " gated with a result %llu, of them he showed %llu (silhouette missed); queries begun %llu, polled %llu, no free slot %llu, closed at present %llu",
        res, static_cast<unsigned>(h.hits.load()), res ? static_cast<double>(h.samplesSum.load()) / static_cast<double>(res) : 0.0, static_cast<unsigned long long>(h.unmatched.load()),
        static_cast<unsigned long long>(h.byHint[0][0].load()), static_cast<unsigned long long>(h.byHint[0][1].load()), static_cast<unsigned long long>(h.byHint[1][0].load()),
        static_cast<unsigned long long>(h.byHint[1][1].load()), static_cast<unsigned long long>(h.byHint[2][0].load()), static_cast<unsigned long long>(h.byHint[2][1].load()),
        static_cast<unsigned long long>(h.gatedResults.load()), static_cast<unsigned long long>(h.gatedShowed.load()), static_cast<unsigned long long>(h.glueBegan.load()),
        static_cast<unsigned long long>(h.gluePolled.load()), static_cast<unsigned long long>(h.glueNoSlot.load()), static_cast<unsigned long long>(h.glueForced.load()));
    if (port_log_summary) port_log_summary("%s", line);
}
void SilhGatePass() {
    int mode = SilhGateMode();
    const int om = SilhOccMode(); // (across26, GXV_SILH_OCC: the silhouette's draws are flagged for the back end's occlusion query; the census mode runs the pass with the gate's census)
    if (!mode && om == 3) mode = 2;
    if (!mode) return;
    ++g_sgCtr.pictures;
    g_sgGated.clear();
    std::vector<Cmd>& cmds = g_buf.cmds;
    const size_t n = cmds.size();
    int efbW = g_ssEfbW ? g_ssEfbW : g.efbW, efbH = g_ssEfbH ? g_ssEfbH : g.efbH;
    const bool stencilOn = ShadowStencilMode() >= 1; // (stencil43: the destination-alpha silhouette is a placed layer too; the gate counts it as a silhouette either way)
    bool in = false, sawCopy = false;
    int vol = 0, sil = 0, unk = 0;
    std::vector<size_t> silCmds; // (stencil43: the window's silhouette draws)
    std::vector<uint8_t> drop;
    for (size_t i = 0; i < n; ++i) {
        const Cmd c = cmds[i];
        switch (c.type) {
        case CMD_SET_TARGET:
            in = false;
            efbW = g_buf.targets[c.arg].efbW;
            efbH = g_buf.targets[c.arg].efbH;
            break;
        case CMD_COPY_DISP: case CMD_PASS_BEGIN: case CMD_PRESENT:
            in = false;
            break;
        case CMD_DRAW: {
            const DrawRec& d = g_buf.draws[c.arg];
            if (d.idxCount == 0) break;
            const int kind = SfAlphaKind(d);
            if (kind == 1 && SsFullCover(d, efbW, efbH)) { in = true; vol = sil = unk = 0; silCmds.clear(); break; }
            if (!in || kind != 2) break;
            int alpha = 0;
            const int cls = SsClassify(d, alpha, stencilOn);
            if (cls == 1 || cls == 2) ++vol;
            else if (cls == 3 || SgSilhouetteShape(d)) { ++sil; silCmds.push_back(i); }
            else ++unk;
            break;
        }
        case CMD_COPY_TEX: {
            CopyTexCmd& t = g_buf.copyTex[c.arg];
            const bool shape = !(t.skip || t.fromPass || t.passMore || t.srcTex || t.dstTexW || t.depth || t.fill || t.texId == 0 || t.clear.enable || t.clearElided ||
                                 CopyReads(t) != CH_A || !SsMatIsAlpha(t) || t.src.x != 0 || t.src.y != 0 || t.src.w != efbW || t.src.h != efbH || t.dstW != t.src.w ||
                                 t.dstH != t.src.h);
            if (!shape) { in = false; break; }
            if (!in) { ++g_sgCtr.why[SG_NOSTART]; break; }
            in = false;
            // the next draw is the fill
            size_t j = i + 1;
            for (; j < n; ++j) {
                const Cmd& x = cmds[j];
                if (x.type == CMD_SHADER || x.type == CMD_PROGRAM || x.type == CMD_VARIANT || x.type == CMD_BUF_UPLOAD) continue;
                if (x.type == CMD_TEX_UPLOAD && g_buf.uploads[x.arg].texId != t.texId) continue;
                break;
            }
            int map = -1, coord = -1, thr = 0;
            bool fillOk = false;
            if (j < n && cmds[j].type == CMD_DRAW) {
                const DrawRec& fd = g_buf.draws[cmds[j].arg];
                float rect[4];
                uint64_t vhash = 0;
                fillOk = SfFillShape(fd, map, coord) && fd.tex[map].texId == t.texId && SsFillState(fd, map) && (thr = SsFillThreshold(fd, map)) &&
                         SfeFillGeom(fd, coord, efbW, efbH, rect, vhash) && rect[0] <= -1.0f + 1e-4f && rect[1] <= -1.0f + 1e-4f && rect[2] >= 1.0f - 1e-4f &&
                         rect[3] >= 1.0f - 1e-4f;
            }
            bool other = false;
            for (size_t q = 0; q < n && !other && fillOk; ++q) {
                if (q == j) continue;
                const Cmd& x = cmds[q];
                if (x.type == CMD_DRAW) {
                    const DrawRec& e = g_buf.draws[x.arg];
                    for (int m = 0; m < 8 && !other; ++m) other = (e.texMask & (1u << m)) && e.tex[m].texId == t.texId;
                } else if (x.type == CMD_COPY_TEX) {
                    other = g_buf.copyTex[x.arg].srcTex == t.texId;
                }
            }
            int why;
            bool act = false;
            if (!fillOk) why = SG_KEEP_FILL;
            else if (other) why = SG_KEEP_READER;
            else if (vol) why = SG_KEEP_VOLUME;
            else if (unk) why = SG_KEEP_UNKNOWN;
            else if (sil == 0) { why = SG_GATED_EXACT; act = true; }
            else if (t.silhHint == 1) { why = SG_GATED_HINT; act = true; }
            else why = SG_KEEP_HINT;
            (void)thr;
            ++g_sgCtr.why[why];
            if (om && sil > 0) { // (GXV_SILH_OCC: the window's silhouette draws are counted by the back end, whether this picture is gated or not: the history must go on)
                for (size_t ix : silCmds) g_buf.draws[cmds[ix].arg].occ = 1;
                if (act && mode == 1 && why == SG_GATED_HINT) SilhOccHistory().NoteGated(g_frame);
            }
            if (act && mode == 1) {
                t.skip = true;
                g_sgGated.push_back(c.arg);
                if (drop.empty()) drop.assign(n, 0);
                drop[j] = 1;
                // (stencil43, both switches: the silhouette's draws of a window the gate leaves out are left out too: with no fill to clear it, its alpha would stay in the EFB
                // where a later draw that blends by destination alpha would read it; the hint says nothing would pass anyway)
                if (stencilOn && sil && t.silhHint == 1 && om == 0) // (with GXV_SILH_OCC the draws stay: their count is what lets the silhouette come back)
                    for (size_t ix : silCmds) drop[ix] = 1;
            }
            sawCopy = true;
            break;
        }
        default:
            break;
        }
    }
    if (!sawCopy && !g_sgCtr.why[SG_NOSTART]) ++g_sgCtr.why[SG_NOWINDOW];
    if (!drop.empty()) {
        size_t out = 0;
        for (size_t i = 0; i < n; ++i)
            if (!drop[i]) cmds[out++] = cmds[i];
        cmds.resize(out);
    }
    static const char* const kName[SG_COUNT] = {"left out exactly (no alpha writer)", "left out on the game's hint", "kept: hint says may show / none", "kept: a shadow volume",
                                                 "kept: unknown alpha writer", "kept: fill shape", "kept: another reader", "kept: capture shape", "no window start (no clear quad)",
                                                 "no capture in the picture"};
    if (g_sgCtr.pictures >= 600) {
        char line[700];
        int at = std::snprintf(line, sizeof line, "silhgate: GXV_SILH_GATE=%d, %llu pictures:", mode, static_cast<unsigned long long>(g_sgCtr.pictures));
        for (int r = 0; r < SG_COUNT; ++r)
            if (g_sgCtr.why[r] && at < static_cast<int>(sizeof line) - 80)
                at += std::snprintf(line + at, sizeof line - at, " %s %llu;", kName[r], static_cast<unsigned long long>(g_sgCtr.why[r]));
        if (port_log_summary) port_log_summary("%s", line);
        if (om) SilhOccLog(om);
        for (int r = 0; r < SG_COUNT; ++r) g_sgTot.why[r] += g_sgCtr.why[r];
        g_sgTot.pictures += g_sgCtr.pictures;
        g_sgCtr = SgCount{};
    }
}
} // namespace
void SilhGateCounters(uint64_t out[16]) {
    out[0] = g_sgTot.pictures + g_sgCtr.pictures;
    for (int r = 0; r < SG_COUNT && 1 + r < 16; ++r) out[1 + r] = g_sgTot.why[r] + g_sgCtr.why[r];
}
// (dread25) The draws that were "unclassified alpha writers" since the last call, then cleared: 8 words each (the log prints them with the shader's
// stable key): [0] fragment shader id, [1] draws, [2] indices, [3] blend | src << 4 | dst << 8 | eq << 12 | dza << 16, [4] alpha test | dst alpha << 1 |
// stages << 2 | colour write << 8 | volumes had begun << 9, [5] depth test | func << 1 | write << 4, [6] constant destination alpha (0..255) + 1, 0 = none, [7] 0.
int ShadowStencilCulprits(uint32_t* out, int cap) {
    int n = 0;
    for (int i = 0; i < g_ssCulN && n < cap; ++i, ++n) {
        const SsCul& c = g_ssCul[i];
        uint32_t* w = out + n * 8;
        w[0] = c.fs;
        w[1] = c.n;
        w[2] = c.idx;
        w[3] = static_cast<uint32_t>(c.blend) | static_cast<uint32_t>(c.src) << 4 | static_cast<uint32_t>(c.dst) << 8 | static_cast<uint32_t>(c.eq) << 12 | static_cast<uint32_t>(c.dza) << 16;
        w[4] = static_cast<uint32_t>(c.alphaTest ? 1 : 0) | static_cast<uint32_t>(c.dstAlpha ? 2 : 0) | static_cast<uint32_t>(c.stages) << 2 | static_cast<uint32_t>(c.colorWrite) << 8 |
               static_cast<uint32_t>(c.afterLayers) << 9;
        w[5] = static_cast<uint32_t>(c.depthTest) | static_cast<uint32_t>(c.depthFunc) << 1 | static_cast<uint32_t>(c.depthWrite) << 4;
        w[6] = static_cast<uint32_t>(c.zAlpha + 1);
        w[7] = c.cause;
    }
    g_ssCulN = 0;
    return n;
}
void ShadowFetchPublish(uint64_t key, int state) {
    for (SfeSlot& s : g_sfeSlots)
        if (s.key.load(std::memory_order_acquire) == key) { s.state.store(state, std::memory_order_release); return; }
    for (SfeSlot& s : g_sfeSlots)
        if (s.key.load(std::memory_order_acquire) == 0) {
            s.state.store(state, std::memory_order_release);
            s.key.store(key, std::memory_order_release);
            return;
        }
}
void ShadowFetchUsable(int on) { g_sfeUsable.store(on, std::memory_order_release); }
void ShadowFetchCounters(uint64_t out[5]) {
    out[0] = g_sfeTotal.pictures + g_sfeCtr.pictures;
    out[1] = g_sfeTotal.pairs + g_sfeCtr.pairs;
    out[2] = g_sfeTotal.fetched + g_sfeCtr.fetched;
    out[3] = g_sfeTotal.requests + g_sfeCtr.requests;
    out[4] = g_sfeTotal.stale + g_sfeCtr.stale;
}

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
    const int smallSide = static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_ATLAS_SIDE", 672))));
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
    const long mode = static_cast<long>(VGX_KNOB(AbsoluteIndices() ? EnvInt("GXV_ATLAS", 2) : 0));
    if (!mode || (g_buf.passes.empty() && g_atlasOf.empty())) return;
    stats::Frame& fr = stats::Cur();
    // (strikers2audit, finding 7 of the Strikers review) the once-a-minute line for GXV_ATLAS: how many passes the frames had and how many the atlas actually took
    // (a rule that rejects every pass leaves the switch on and doing nothing: the line then says "0 taken"). Only while the switch is on; the front end's thread.
    struct AtlasCensus {
        stats::Frame& f;
        uint32_t p0, r0, m0, e0, passes;
        long mode;
        AtlasCensus(stats::Frame& fr_, size_t n, long m) : f(fr_), p0(fr_.atlasPasses), r0(fr_.atlasRemaps), m0(fr_.atlasMerged), e0(fr_.atlasEvicted), passes(static_cast<uint32_t>(n)), mode(m) {}
        ~AtlasCensus() {
            static unsigned long long frames = 0, seen = 0, taken = 0, remaps = 0, merged = 0, evicted = 0;
            static auto since = FeClock::Picture();
            ++frames;
            seen += passes;
            taken += f.atlasPasses - p0;
            remaps += f.atlasRemaps - r0;
            merged += f.atlasMerged - m0;
            evicted += f.atlasEvicted - e0;
            const auto now = FeClock::Picture();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - since).count() < 60) return;
            since = now;
            if (port_log_summary)
                port_log_summary("atlas: GXV_ATLAS=%ld, the last minute: %llu pictures with passes, %llu passes in them, %llu drawn into an atlas cell (%.1f %%), %llu draws redirected, %llu merged, "
                                 "%llu pictures taken out again%s",
                                 mode, frames, seen, taken, seen ? 100.0 * static_cast<double>(taken) / static_cast<double>(seen) : 0.0, remaps, merged, evicted,
                                 seen > 0 && taken == 0 ? " (THE SWITCH TOOK NOTHING)" : "");
            frames = seen = taken = remaps = merged = evicted = 0;
        }
    } atlasCensus(fr, g_buf.passes.size(), mode);
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
    // Passes of a chain (a pass whose draws sample the picture an earlier pass made, as the
    // bloom's blur chain does) stay out of the atlas: the later pass cannot draw into the page
    // it samples, and sampling a cell through shifted texture coordinates (AtlasRedirect
    // refuses those) would take the picture out of the page again, with a copy scene of its own.
    static std::vector<uint8_t> passChained;
    passChained.assign(g_buf.passes.size(), 0);
    {
        uint32_t open = 0xFFFFFFFFu;
        size_t openAt = 0;
        struct Made { uint32_t texId; uint32_t pass; };
        Made made[16];
        int nMade = 0;
        for (size_t ci = 0; ci < cmds.size(); ++ci) {
            const Cmd& c = cmds[ci];
            if (c.type == CMD_PASS_BEGIN) { open = c.arg; openAt = ci; }
            else if (c.type == CMD_COPY_TEX) {
                if (open != 0xFFFFFFFFu) {
                    passCopy[open] = c.arg;
                    Kind k;
                    if (classify(g_buf.passes[open], c.arg, k)) {
                        int i = 0;
                        while (i < nKinds && !sameKind(kinds[i], k)) ++i;
                        if (i == nKinds && nKinds < 8) kinds[nKinds++] = k;
                        int src = -1;
                        // only with GXV_BLOOM_FOLD on (or GXV_ATLAS_NOCHAIN=1): the default path keeps the old atlas choice
                        const bool chainRule = port::stageknob::Int("GXV_BLOOM_FOLD", 0) != 0 || (port::stageknob::Int("GXV_GLOW26", 0) != 0 && port::glow26::ClampLevel(port::stageknob::Int("GXV_GLOW26", 0)) < port::glow26::kQ) || port::stageknob::Int("GXV_ATLAS_NOCHAIN", 0) != 0;
                        for (size_t j = openAt + 1; chainRule && j < ci && src < 0; ++j) {
                            if (cmds[j].type != CMD_DRAW) continue;
                            const DrawRec& d = g_buf.draws[cmds[j].arg];
                            for (int m = 0; m < 8 && src < 0; ++m)
                                if (d.texMask & (1u << m))
                                    for (int q = 0; q < nMade; ++q)
                                        if (made[q].texId == d.tex[m].texId) { src = q; break; }
                        }
                        if (src >= 0) {
                            passChained[open] = 1;
                            const uint32_t sp = made[src].pass;
                            if (!passChained[sp]) {
                                passChained[sp] = 1;
                                Kind ks;
                                if (classify(g_buf.passes[sp], passCopy[sp], ks)) {
                                    int q = 0;
                                    while (q < nKinds && !sameKind(kinds[q], ks)) ++q;
                                    if (q < nKinds && kinds[q].n) --kinds[q].n;
                                }
                            }
                        } else if (i < nKinds) {
                            ++kinds[i].n;
                        }
                        if (nMade < 16) made[nMade++] = Made{g_buf.copyTex[c.arg].texId, open};
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
            bool ok = classify(ps, copy, k) && !passChained[c.arg];
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
    const bool noMerge = VGX_KNOB(EnvInt("GXV_NOMERGE", 0) != 0) != 0;
    size_t n = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        const Cmd c = out[i];
        if (!noMerge && fr.atlasRemaps && c.type == CMD_DRAW && n > 0 && out[n - 1].type == CMD_DRAW) {
            DrawRec& a = g_buf.draws[out[n - 1].arg];
            const DrawRec& b = g_buf.draws[c.arg];
            if (a.vbuf == 0 && b.vbuf == 0 && a.vchunk == b.vchunk && a.vtxOffset == b.vtxOffset && a.prim == b.prim && a.layoutMask == b.layoutMask &&
                a.vsId == b.vsId && a.fsId == b.fsId && a.texMask == b.texMask && a.idxOffset + a.idxCount == b.idxOffset &&
                a.depthMark == b.depthMark && a.hud == b.hud && a.owner == b.owner && a.playerMark == b.playerMark &&
                a.vtxFirst + a.vtxCount == b.vtxFirst && a.vtxCount + b.vtxCount <= 0xFFFF &&
                std::memcmp(a.ublock, b.ublock, sizeof a.ublock) == 0 && std::memcmp(&a.rs, &b.rs, sizeof(RenderState)) == 0 &&
                std::memcmp(a.tex, b.tex, sizeof a.tex) == 0 && a.stOp == b.stOp && a.occ == b.occ) {
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
    // Pixels of r that do not hold `id` become unknown (a draw that writes `id` to some of them).
    void Keep(int plane, const EfbRect& r, uint8_t id) {
        int x0, y0, x1, y1;
        if (!Clip(r, x0, y0, x1, y1)) return;
        for (int y = y0; y < y1; ++y) {
            uint8_t* row = &px[plane][static_cast<size_t>(y) * kChkW];
            for (int x = x0; x < x1; ++x)
                if (row[x] != id) row[x] = 0;
        }
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
            const DrawRec& dr = g_buf.draws[c.arg];
            const RenderState& rs = dr.rs;
            const EfbRect sc{rs.scX, rs.scY, rs.scW, rs.scH};
            uint8_t w = DrawWrites(rs);
            if ((w & CH_A) && ShadowFillOn()) {
                // (GXV_SHADOWFILL_BOUNDS: alpha as ShadowFillPass models it, so that its fills are checked pixel by pixel: not written,
                // the constant 0 written to some pixels of the scissor, or anything written to its own pixels only)
                const int kind = SfAlphaKind(dr);
                if (kind == 1 && (dr.cover || dr.coverCull)) g_chk.Set(1, EfbRect{dr.exX, dr.exY, dr.exW, dr.exH}, g_chk.Id(1, 0.0f));
                else if (kind == 1) g_chk.Keep(1, sc, g_chk.Id(1, 0.0f));
                else if (kind == 2 && dr.ab) g_chk.Set(1, EfbRect{dr.abX, dr.abY, dr.abW, dr.abH}, 0);
                if (kind != 2 || dr.ab) {
                    w = static_cast<uint8_t>(w & ~CH_A);
                    g_chk.lastDrawValid = false;
                }
            }
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
// ==================================================================== prewarm24: the warm-up scope (GXV_PREWARM)
// The game (decomp JPAEmitterManager::draw, PortPrewarmGroup) draws effects nobody has emitted yet, once, between PORT_GX_MARK_WARM_BEGIN
// and PORT_GX_MARK_WARM_END (BP 0xFF kinds 14 / 15), with its own GX library state saved before and put back after. The front end:
//   - at the start keeps a copy of everything a draw's resolution reads or leaves: the registers (State g), the resolved state (cur, the key
//     memo's current key), the uniform blocks' offsets and staleness, the pending palette row, the templated J3D resolution's deferred copies
//     (made first: dj::SyncKeys) and its armed template;
//   - inside, resolves every draw exactly as outside (the same functions, the same registers as the game set them, so the same keys: the
//     key, the render state and the folding are made of the registers and the draw's vertices only), but builds the blocks into a scratch
//     arena, records nothing, and queues each draw's program variant (WarmNote above);
//   - at the end puts the copy back, with two changes that only make caches look again: the vertex-format plans are marked stale and the
//     descriptor generation moves on to a number never used (nothing keyed by a generation can then match a format the scope set).
// So the stream after the scope is decoded from the very state it would have been without the scope: the same records, vertices, indices and
// blocks. Only the caches (shader registry, key memo, textures uploaded for the scope's draws, the warm-up list file) differ, as after any draw.
// The shader and texture ids of later draws can differ (made in another order); what they stand for does not.
namespace {
struct WarmSnap {
    State g;
    Resolved cur;
    CurKey curKey;
    bool blocksStale = true;
    uint32_t pendingPosMtxMax = 0xFF;
    J3n2MirFlags mir;
    const dj::Keys* armed = nullptr;
    uint32_t armedHandle = 0;
    const void* armedList = nullptr;
};
std::unique_ptr<WarmSnap> g_warmSnap;
} // namespace
long PrewarmMode() {
    static const long m = [] {
        const char* e = std::getenv("GXV_PREWARM");
        const long v = e ? std::strtol(e, nullptr, 10) : 0;
        return v < 0 || v > 2 ? 0L : v;
    }();
    return m;
}
void WarmScopeMarker(uint32_t kind) {
    if (kind == 14u) {
        if (g_warmScope || !WarmScopeHonoured()) { ++g_pw.unmatched; return; }
        dj::SyncKeys(); // (cur.fs / cur.vs hold the keys of cur's ids from here on: the copy below is whole)
        if (!g_warmSnap) g_warmSnap = std::make_unique<WarmSnap>();
        WarmSnap& s = *g_warmSnap;
        s.g = g;
        s.cur = cur;
        s.curKey = g_curKey;
        s.blocksStale = g_blocksStale;
        s.pendingPosMtxMax = g_pendingPosMtxMax;
        s.mir = g_j3n2Mir;
        s.armed = dj::g_armed;
        s.armedHandle = dj::g_armedHandle;
        s.armedList = dj::g_armedList;
        g_warmUni.clear();
        for (int b = 0; b < UB_COUNT; ++b) { g_warmBlockOff[b] = kNoBlock; g_warmBlockLen[b] = 0; }
        g_blocksStale = true; // (the scratch arena holds nothing)
        g_warmScope = true;
        ++g_pw.scopes;
        return;
    }
    if (!g_warmScope || !g_warmSnap) { ++g_pw.unmatched; return; }
    WarmSnap& s = *g_warmSnap;
    const uint32_t gen = std::max(g.vcdGen, s.g.vcdGen);
    g = s.g;
    g.vcdGen = gen + 1;    // (a generation no format of the scope had)
    g.layoutDirty = true;  // (the plans of the scope's formats are made again from the registers put back)
    cur = s.cur;
    g_curKey = s.curKey;
    g_blocksStale = s.blocksStale;
    g_pendingPosMtxMax = s.pendingPosMtxMax;
    g_j3n2Mir = s.mir;
    dj::g_armed = s.armed;
    dj::g_armedHandle = s.armedHandle;
    dj::g_armedList = s.armedList;
    g_warmScope = false;
}
bool WarmScopeActive() { return g_warmScope; }
// precomp26 GXV_STAGE_ENUM (read once; 0 off, the default; 1 census: the dry draws are made and counted, nothing is linked or compiled; 2 on).
long StageEnumMode() {
    static const long m = [] {
        const char* e = std::getenv("GXV_STAGE_ENUM");
        const long v = e ? std::strtol(e, nullptr, 10) : 0;
        return v < 0 || v > 2 ? 0L : v;
    }();
    return m;
}
// PORT_GX_MARK_STAGE_ENUM_DRAW (BP 0xFF kind 33; arg = bits 8-23): one draw of a J3D shape that the game did not send, inside a warm-up scope whose state the game made with the
// material's display list, the shape's vertex description list and the current-matrix words. bits 0-7 of the arg: the highest position-matrix row the shape's vertices name
// (0xFF: none / not known); bits 8-15: the vertex count of the shape's smallest primitive (0: not known). The draw is resolved by the very code a real draw runs (ResolveState inside
// EmitDraw: BuildFsKey / BuildVsKey / Lookup / BuildRenderState / the folding of small draws / the palette twin), with vertices of zeros of the layout the descriptor plans,
// once with a count above GXV_SOFTXFORM (what a large shape is) and, when the shape has a primitive of GXV_SOFTXFORM vertices or fewer, once with that count (a small draw is
// CPU-transformed and may fold its registers: another fragment shader and layout). Nothing is recorded, no texture is looked up.
void StageEnumDrawMarker(uint32_t arg) {
    ++g_enc.markers;
    if (!g_warmScope || StageEnumMode() == 0) { ++g_enc.outsideScope; return; }
    ++g_enc.seq;
    const Plan& plan = CurrentPlan(0); // (J3D writes every shape's vertex format into GX_VTXFMT0)
    if (!plan.valid || plan.streamSize == 0) { ++g_enc.noPlan; return; }
    const uint32_t row = arg & 0xFFu, smallCnt = (arg >> 8) & 0xFFu;
    const uint32_t big = ((SoftXformLimit() + 3u) / 3u + 1u) * 3u;
    uint32_t counts[2] = {big, 0};
    unsigned n = 1;
    if (smallCnt && smallCnt <= SoftXformLimit()) { counts[n++] = std::max(3u, (smallCnt / 3u) * 3u); ++g_enc.small; }
    for (unsigned i = 0; i < n; ++i) {
        uint8_t* out = BeginVertices(plan.layoutMask, counts[i]);
        if (!out) return;
        std::memset(out, 0, static_cast<size_t>(counts[i]) * plan.layout.stride);
        SetPendingPosMtxMax(row);
        g_enumDry = true;
        EmitDraw(0x90, plan.layoutMask, counts[i]);
        g_enumDry = false;
        ++g_enc.draws;
    }
}
// (tests, the back end's census line) [0] markers, [1] outside a scope, [2] without a plan, [3] draws resolved, [4] variants handed to the back end (new to the stage), [5] repeats, [6] small-draw twins
void StageEnumCounters(uint64_t out[7]) {
    out[0] = g_enc.markers; out[1] = g_enc.outsideScope; out[2] = g_enc.noPlan; out[3] = g_enc.draws; out[4] = g_enc.variants; out[5] = g_enc.dup; out[6] = g_enc.small;
}
#include "vgx_bglayer2.inc" // (bglayer25: GXV_BGLAYER=1, the far-background layer cache; after WarmSnap)

// ==================================================================== layoutwarm24: layouts (GXV_LAYOUTWARM, GXV_LAYOUTWARM_LOG)
// GXV_LAYOUTWARM=1 (default 0): the game (decomp Game/Screen/LayoutManager.cpp) draws each loaded layout once behind the closed screen, between
// the prewarm24 scope markers; this front end honours the scope for it as for the effects (WarmScopeHonoured). GXV_LAYOUTWARM_LOG=1 (default 0):
// the game brackets every layout draw in play with PORT_GX_MARK_LAYOUT_BEGIN / _END (kinds 19 / 20, the layout's id in bits 8-23); the first such
// pair of a layout that has draws logs "layoutwarm: first draw ...": its draws, its program variants, of them those the warm-up list did not know
// (g_varSeen - the shader list: a draw's NoteVariant and the scopes' WarmNote put every variant there when a list is configured - and g_pwSent, the
// scopes' own; a variant in neither, and not drawn by an earlier layout, is one the back end links on demand, unless it is linked already by another way), the shaders (vertex / fragment) and the textures this front end made for it. The counters are
// per draw in stream order, on the front end's thread: no cost without the switch (the game writes no marker).
long LayoutWarmMode() {
    static const long m = [] {
        const char* e = std::getenv("GXV_LAYOUTWARM");
        const long v = e ? std::strtol(e, nullptr, 10) : 0;
        return v < 0 || v > 1 ? 0L : v;
    }();
    return m;
}
bool LayoutCensusOn() {
    static const bool on = [] {
        const char* e = std::getenv("GXV_LAYOUTWARM_LOG");
        return e && std::strtol(e, nullptr, 10) != 0;
    }();
    return on;
}
// dive27 GXV_REHEARSE (read once; 0 off, the default; 1 on): the game's rehearsal entries (decomp Game/System/PortRehearse.cpp) draw in the warm-up scope. 2 also asks entry.cpp to fill in
// GXV_PREWARM=1 and GXV_LAYOUTWARM=1 (effects and layouts: the other first-use paths that exist already).
long RehearseMode() {
    static const long m = [] {
        const char* e = std::getenv("GXV_REHEARSE");
        const long v = e ? std::strtol(e, nullptr, 10) : 0;
        return v < 0 || v > 2 ? 0L : v;
    }();
    return m;
}
bool WarmScopeHonoured() { return PrewarmMode() != 0 || LayoutWarmMode() != 0 || StageEnumMode() != 0 || RehearseMode() != 0; }
extern "C" const char* port_layoutwarm_name(unsigned id) __attribute__((weak));
extern "C" uint32_t port_scene_serial(void) __attribute__((weak));
namespace {
struct LwCen {
    uint32_t id = 0, draws = 0, newVars = 0;
    std::vector<uint64_t> vars;
    uint32_t fs0 = 0, vs0 = 0, tex0 = 0, texB0 = 0;
    uint32_t serial = ~0u, stage = 0, lines = 0;
    std::vector<uint8_t> done; // per id: a line was logged
    uint64_t layouts = 0;
    std::unordered_set<uint64_t> known; // variants an earlier census draw had (the back end links a program once)
} g_lwc;
void LwCensusDraw(uint16_t vsId, uint16_t fsId, uint32_t layoutMask, uint32_t blendKey) {
    ++g_lwc.draws;
    const uint64_t vk = VariantKey(vsId, fsId, layoutMask, blendKey);
    for (uint64_t x : g_lwc.vars) if (x == vk) return;
    g_lwc.vars.push_back(vk);
    if (!g_varSeen.count(vk) && !g_pwSent.count(vk) && g_lwc.known.insert(vk).second) ++g_lwc.newVars;
}
} // namespace
void LayoutCensusMarker(uint32_t kind, uint32_t id) {
    if (!LayoutCensusOn()) return;
    if (kind == 19u) {
        const uint32_t serial = port_scene_serial ? port_scene_serial() : 0;
        const uint32_t stage = port_scene_key ? port_scene_key() : 0;
        if (serial != g_lwc.serial || stage != g_lwc.stage) { g_lwc.serial = serial; g_lwc.stage = stage; g_lwc.done.clear(); g_lwc.known.clear(); g_lwc.lines = 0; }
        if (g_warmScope || id == 0 || id >= 4096) return;
        if (g_lwc.done.size() <= id) g_lwc.done.resize(id + 1, 0);
        if (g_lwc.done[id]) return; // (its first draw with draws was logged)
        const stats::Frame& fr = stats::Cur();
        g_lwc.id = id; g_lwc.draws = 0; g_lwc.newVars = 0; g_lwc.vars.clear();
        g_lwc.fs0 = fr.shadersFs; g_lwc.vs0 = fr.shadersVs; g_lwc.tex0 = fr.texUploads; g_lwc.texB0 = fr.texUploadBytes;
        g_lwCensus = true;
        return;
    }
    if (!g_lwCensus) return;
    g_lwCensus = false;
    if (g_lwc.draws == 0) return; // (nothing drawn yet: the layout's next draw is its first)
    const stats::Frame& fr = stats::Cur();
    const uint32_t nfs = fr.shadersFs >= g_lwc.fs0 ? fr.shadersFs - g_lwc.fs0 : 0, nvs = fr.shadersVs >= g_lwc.vs0 ? fr.shadersVs - g_lwc.vs0 : 0;
    const uint32_t ntex = fr.texUploads >= g_lwc.tex0 ? fr.texUploads - g_lwc.tex0 : 0, nb = fr.texUploadBytes >= g_lwc.texB0 ? fr.texUploadBytes - g_lwc.texB0 : 0;
    g_lwc.done[g_lwc.id] = 1;
    ++g_lwc.layouts;
    static const unsigned maxLines = [] { const char* e = std::getenv("GXV_LAYOUTWARM_LINES"); return e ? static_cast<unsigned>(std::max(0L, std::strtol(e, nullptr, 10))) : 200u; }();
    const bool any = g_lwc.newVars || nfs || nvs || ntex;
    if (g_lwc.lines >= maxLines || !port_log_summary) return;
    ++g_lwc.lines;
    const char* name = port_layoutwarm_name ? port_layoutwarm_name(g_lwc.id) : nullptr;
    port_log_summary("layoutwarm: first draw: stage %08x layout #%u %s: %u draws, %u program variants (%u not in the warm-up list), %u new vertex / %u new fragment "
                     "shaders, %u new textures (%.1f KiB)%s",
                     g_lwc.stage, g_lwc.id, name ? name : "?", g_lwc.draws, static_cast<unsigned>(g_lwc.vars.size()), g_lwc.newVars, nvs, nfs, ntex, nb / 1024.0,
                     any ? "  <- paid in play" : "");
}

void SetShaderWarmupDeferred(bool deferred) { g_warmDeferred = deferred; }
#if GXV_RECIPE_SEED_BATCH
void SetShaderRecipeSeedFile(const char* path) { g_recipeSeedPath = path ? path : ""; }
#endif

#include "vgx_texmag.inc" // (hdcensus: GXV_TEXMAG_CENSUS)
static void (*g_frameSink)(CmdBuf&) = nullptr;
void SetFrameSink(void (*sink)(CmdBuf& buf)) { g_frameSink = sink; }

// GXV_BLOOM_BRACKET: the pass (see vgx_bloombracket.h for what makes dropping the three commands an identity). Runs at the picture's end before every other pass, on the commands
// as the game made them: the capture (CopyTexCmd::bracket), then the fill's draw and the put-back's draw (DrawRec::owner), with nothing but resource commands between.
static void BloomBracketPass() {
    const int mode = BloomBracketMode();
    if (mode == 0 || g_bb.seq == 0) { g_bb = BbFront{}; return; }
    g_bbCensus.Picture(true);
    std::vector<Cmd>& cmds = g_buf.cmds;
    std::vector<char> gone;
    const bool hasAlpha = EfbHasAlpha();
    for (size_t i = 0; i < cmds.size(); ++i) {
        if (cmds[i].type != CMD_COPY_TEX || cmds[i].arg >= g_buf.copyTex.size()) continue;
        const CopyTexCmd& c = g_buf.copyTex[cmds[i].arg];
        if (c.bracket == 0) continue;
        bb::Inputs in;
        in.mode = mode;
        in.dead = ((g_bb.deadMask >> c.bracket) & 1ull) != 0;
        in.efbAlpha = hasAlpha;
        in.hrw = HrwMode();
        in.host = bb::HostBlock().load(std::memory_order_relaxed);
        in.copy.present = true;
        in.copy.texId = c.texId;
        in.copy.depth = c.depth; in.copy.fromPass = c.fromPass; in.copy.srcTex = c.srcTex != 0; in.copy.dstTex = c.dstTexW != 0; in.copy.fill = c.fill; in.copy.skip = c.skip;
        in.copy.clearEnable = c.clear.enable;
        in.copy.gxFmt = c.gxFmt;
        in.copy.x = c.src.x; in.copy.y = c.src.y; in.copy.w = c.src.w; in.copy.h = c.src.h;
        in.copy.dstW = c.dstW; in.copy.dstH = c.dstH;
        {
            bool id = true;
            for (int r = 0; r < 4; ++r) {
                const bool alphaRow = r == 3;
                bool same = c.add[r] == 0.0f;
                for (int col = 0; col < 4; ++col) same = same && c.mat[r][col] == (r == col ? 1.0f : 0.0f);
                if (alphaRow && !same && !hasAlpha) { // (an EFB without alpha: the copy reads 1 there)
                    same = c.add[3] == 1.0f && c.mat[3][0] == 0.0f && c.mat[3][1] == 0.0f && c.mat[3][2] == 0.0f && c.mat[3][3] == 0.0f;
                }
                id = id && same;
            }
            in.copy.identity = id;
        }
        if (in.dead) {
            // the fill's draw and the put-back's draw, and what lies between
            const unsigned idv = c.bracket;
            size_t fillAt = SIZE_MAX, putAt = SIZE_MAX;
            bool foreign = false;
            for (size_t j = i + 1; j < cmds.size() && putAt == SIZE_MAX; ++j) {
                const Cmd& k = cmds[j];
                if (k.type == CMD_DRAW && k.arg < g_buf.draws.size()) {
                    const uint16_t o = g_buf.draws[k.arg].owner;
                    if (bb::IsOwnerTag(o) && bb::OwnerId(o) == idv && bb::OwnerRole(o) == 1 && fillAt == SIZE_MAX) fillAt = j;
                    else if (bb::IsOwnerTag(o) && bb::OwnerId(o) == idv && bb::OwnerRole(o) == 2 && fillAt != SIZE_MAX) putAt = j;
                    else foreign = true;
                } else if (k.type == CMD_COPY_TEX || k.type == CMD_COPY_DISP || k.type == CMD_PASS_BEGIN || k.type == CMD_SET_TARGET || k.type == CMD_PRESENT ||
                           (k.type == CMD_TEX_FREE && k.arg == c.texId)) {
                    foreign = true;
                }
            }
            in.markersOk = fillAt != SIZE_MAX && putAt != SIZE_MAX;
            in.foreignBetween = foreign;
            const auto facts = [&](size_t at, bb::DrawFacts& f) {
                const DrawRec& d = g_buf.draws[cmds[at].arg];
                f.present = true;
                f.cover = d.cover != 0;
                f.blend = d.rs.blend != 0; f.depthTest = d.rs.depthTest != 0; f.depthWrite = d.rs.depthWrite != 0;
                f.colorWrite = d.rs.colorWrite != 0; f.alphaWrite = d.rs.alphaWrite != 0;
                f.dstAlphaZero = d.rs.dstAlphaZero; f.stOp = d.stOp;
                f.x = d.exX; f.y = d.exY; f.w = d.exW; f.h = d.exH;
            };
            if (in.markersOk) {
                facts(fillAt, in.fill);
                facts(putAt, in.put);
                // another reader of the capture's texture anywhere in the picture (the put-back is the only one a dead bracket has)
                bool shared = false;
                for (size_t j = 0; j < cmds.size() && !shared; ++j) {
                    if (cmds[j].type == CMD_DRAW && j != putAt && cmds[j].arg < g_buf.draws.size()) {
                        const DrawRec& d = g_buf.draws[cmds[j].arg];
                        for (int m = 0; m < 8 && !shared; ++m)
                            if ((d.texMask & (1u << m)) && d.tex[m].texId == c.texId) shared = true;
                    } else if (cmds[j].type == CMD_COPY_TEX && cmds[j].arg < g_buf.copyTex.size() && g_buf.copyTex[cmds[j].arg].srcTex == c.texId) {
                        shared = true;
                    }
                }
                in.sharedReader = shared;
                if (bb::Decide(in) == bb::kDropped && mode == 1) {
                    if (gone.empty()) gone.assign(cmds.size(), 0);
                    gone[i] = gone[fillAt] = gone[putAt] = 1;
                }
            }
        }
        g_bbCensus.Note(bb::Decide(in), &in);
    }
    if (!gone.empty()) {
        size_t w = 0;
        for (size_t r = 0; r < cmds.size(); ++r)
            if (!gone[r]) cmds[w++] = cmds[r];
        cmds.resize(w);
        g_lastDraw = -1;
    }
    g_bb = BbFront{};
    char line[1100];
    static const long logEvery = std::clamp<long>(EnvInt("GXV_BLOOM_BRACKET_LOG", 300), 0, 1000000); // (brackets between two log lines; 0 none)
    if (g_bbCensus.Line(line, sizeof line, static_cast<uint64_t>(logEvery), mode) > 0) {
        if (port_log_summary) port_log_summary("%s", line);
        else std::fprintf(stderr, "[vgx] %s\n", line);
    }
}

void FrontPresent(bool direct, void (*onDecoded)()) {
    J3n2Flush(); // (GXV_J3DNATIVE=4 / 5: a chain still active at the picture's end ends here)
    g_depthMarkNext = false; // (hrw41, c183fix F7: every picture ends here, with or without a display copy; its mark never reaches the next picture)
    g_playerMarkDepth = 0;
    if (!g_shaderListDone) EnsureShaderList();
    TexCacheEndFrame();
    Fe24Log();
    DynCenLog();
    PalJoinLog(); // (paljoin31: the "pal-census:" lines every 5 s with GXV_PALJOIN_CENSUS)
    Cache24Log();
    CullLog(); // (ta23: the "ta23:" line every 10 s)
    ClusterLog(); // (vertex25: the "vtx25:" line every 10 s with GXV_CLUSTERCULL)
    Ta27Log(); // (ta27: the "ta27:" line every 10 s with GXV_TA27)
    TinyRunLog(); // (dread25: the "tinyrun25:" line every 10 s with GXV_TINYRUN)
    FaceCullLog(); // (farlod32: GXV_FACECULL's delayed frees, and the "facecull:" line every 10 s)
    PixLog(); // (pix23: the "pix23:" line every 10 s while GXV_PIX_ZERO / GXV_PIX_ATPASS is on)
    // (front-end split: the finishing thread's records, indices and uniforms join the frame here; a no-op when the
    // host did it at the display copy already)
    if (split::g_on) split::FinishFrame(g_buf);
    if (RigidMergeLearnMax()) {
        // (GXV_RIGIDMERGE_DLLEARN: what the frame's cached lists did, for the next frame's copies)
        DlLearnFrameEnd(split::g_on ? split::FinishedJoinLog() : g_joinLogMain, g_frame);
        g_joinLogMain.clear();
    }
    g_jobSeq = 0;
    g_lastJobKey = 0;
    g_hashedSerial = ~0u;
    arena::SoftFrameEnd(g_frame); // (GXV_ARENA_SOFT: the frame's count restarts; the census line)
    g_buf.cmds.push_back(Cmd{CMD_PRESENT, 0});
    g_buf.frame = g_frame;
    g_buf.crowdActive = StaticCrowdEnabled();
    if (g_buf.crowdActive) {
        g_buf.crowdObservations = g_crowdObserved ? 1u : 0u;
        g_buf.crowdTags = g_crowdTagCount;
        g_buf.crowdInvalid = g_crowdInvalid;
    }
    BloomBracketPass(); // (GXV_BLOOM_BRACKET: a dead bloom bracket's capture, fill and put-back, before every other pass looks at the picture)
    if (ShadowStencilMode() >= 1 && SilhGateMode() == 1) {
        // (stencil43: both switches: the gate first, for the windows it leaves out whole (capture AND fill: a picture with no volume), then the stencil takes what is left,
        // the volume windows included, and drops the silhouette's draws there when the hint says it cannot show)
        SilhGatePass();
        ShadowStencilPass();
    } else {
        ShadowStencilPass(); // (GXV_SHADOW_STENCIL: before the fill bounds, which then see a capture left out; the passes verify it below)
        SilhGatePass(); // (GXV_SILH_GATE: after the stencil, which takes the windows it can; before the fill bounds)
    }
    ShadowFillPass(); // (GXV_SHADOWFILL_BOUNDS: before the dead copies and pass isolation, which use what it finds)
    ShadowFetchPass(); // (GXV_SHADOWFETCH: after the fill bounds (their cuts and left-out fills), before the dead copies and passes)
    MarkDeadCopies();
    MarkTransientCopies(); // (GXV_INDCAP_SKIP: after the dead copies, before the passes and the swaps)
    PrevCapPass(); // (GXV_PREVCAP: the marked capture answered by the previous picture; after the marks above, before the passes and the swaps)
    IsolatePasses();
    AtlasPasses();
    ShadowStencilVerify(); // (GXV_SHADOW_STENCIL: a window cut by a pass is undone)
    MarkSwapCopies(); // (GXV_EFBSWAP2: after the passes, which make copies fromPass)
    SilhFusePass(); // (GXV_SILH_FUSE: after the swaps are marked: it fuses a swapped capture with the fill that follows it)
    CapReadLog(); // (GXV_CAPREAD, water23: diagnostic log of the screen captures' readers; reads only)
    VerifyPasses();
    CountScenes();
    GeoOcclFrameEnd(); // (geopass23: GXV_GEOPASS's census of the picture; GXV_OCCLCULL's occluders forgotten, its line)
    if ((BgLayerMode() & 2) != 0) BgFrameEnd(); // (bglayer25: GXV_BGLAYER's census line)
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
    if (workload::FeSplitOn() && g_frame % 300 == 0) {
        // GXV_TEXBAKE census (per picture over the last 300): what the switch removes ("only the texture matrices differ" with it off;
        // draws baked with a matrix of their own with it on), and the other single-block reasons draws stay apart
        static uint64_t prevFrame = 0, prevDraws = 0, prevNew = 0;
        static uint32_t prevMiss[4] = {};
        const double n = static_cast<double>(g_frame - prevFrame);
        uint32_t miss[4];
        for (int k = 0; k < 4; ++k) miss[k] = g_blockMiss[k].load(std::memory_order_relaxed);
        std::fprintf(stderr, "[vgx] texbake: per picture: uniform-block merge failures: only the texture matrices %.1f, only the model-view %.1f, only the TEV "
                             "registers %.1f, several or others %.1f; GXV_TEXBAKE=%d: draws baked %.1f, of them with a texture matrix of their own %.1f\n",
                     (miss[0] - prevMiss[0]) / n, (miss[1] - prevMiss[1]) / n, (miss[2] - prevMiss[2]) / n, (miss[3] - prevMiss[3]) / n, TexBakeOn() ? 1 : 0,
                     (g_texBakeDraws - prevDraws) / n, (g_texBakeNew - prevNew) / n);
        prevFrame = g_frame; prevDraws = g_texBakeDraws; prevNew = g_texBakeNew;
        for (int k = 0; k < 4; ++k) prevMiss[k] = miss[k];
    }
    if (LightNormCensusOn() && (g_frame == 1 || g_frame % 600 == 0))
        std::fprintf(stderr, "[vgx] light directions up to frame %llu: %llu blocks built; %llu non-zero directions written as zero (%llu of them non-finite, %llu not read by the shader at all); %llu kept (%llu non-finite)\n",
                     (unsigned long long)g_frame, (unsigned long long)g_lnc.blocks, (unsigned long long)g_lnc.zeroed, (unsigned long long)g_lnc.zeroedNonFinite,
                     (unsigned long long)g_lnc.zeroedUnread, (unsigned long long)g_lnc.kept, (unsigned long long)g_lnc.keptNonFinite);
    if (PaletteRowsMode() == 2 && (g_frame == 1 || g_frame % 600 == 0))
        std::fprintf(stderr, "[vgx] palette rows up to frame %llu: %llu draws with a per-vertex matrix; highest row 0-2: %llu, 3-5: %llu, 6-8: %llu, 9-11: %llu, 12-14: %llu, 15-17: %llu, 18-20: %llu, 21-23: %llu, 24-26: %llu, 27-29: %llu; above 27: %llu; not known: %llu\n",
                     (unsigned long long)g_frame, (unsigned long long)g_palCensus.draws, (unsigned long long)g_palCensus.byMax[0], (unsigned long long)g_palCensus.byMax[1],
                     (unsigned long long)g_palCensus.byMax[2], (unsigned long long)g_palCensus.byMax[3], (unsigned long long)g_palCensus.byMax[4], (unsigned long long)g_palCensus.byMax[5],
                     (unsigned long long)g_palCensus.byMax[6], (unsigned long long)g_palCensus.byMax[7], (unsigned long long)g_palCensus.byMax[8], (unsigned long long)g_palCensus.byMax[9],
                     (unsigned long long)g_palCensus.beyond, (unsigned long long)g_palCensus.unknown);
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
    J3nPictureEnd(); // (GXV_J3DNATIVE: the census's picture counters, its log line every 300 pictures)
    ++g_frame;
    g_traceNow = -1; // (TraceFrame: asked again for the new frame)
    // Everything of this frame has been read from guest memory: the game
    // thread may go on while the back end draws.
    if (onDecoded) onDecoded();
    TexMagFrame(); // (GXV_TEXMAG_CENSUS: reads the finished buffer; nothing when off)
    if (g_frameSink) g_frameSink(g_buf);
    else if (direct) backend::Execute(g_buf);
    else SubmitToBackend(g_buf);
    g_buf.clear();
    if (StaticCrowdEnabled()) ResetCrowdTags();
    g_blocksStale = true;
    for (uint32_t& o : g_blockOff) o = kNoBlock;
    g_lastDraw = -1;
    split::FrameBegin(); // (whether the next frame runs split)
#if !defined(VGX_CLONE_B)
    if (vgx2_mode) two::PictureEnd(); // (GXV_FE_TWOCORE: the second worker lets go of the picture and gets the next one's state)
#endif
    // Release the producer and submit GPU work before checkpointing newly learned recipes.
    ListFlush();
}

// GXV_HRW (hrw27): the game's partial draw for a pair's second picture is over. The buffer is FrontPresent's, handed on without a picture: it has no display copy and no present, takes no frame number
// (the next picture is still the next picture), runs none of the picture-level passes (they plan a whole picture's copies and shadows; the partial draw has none) and does not age the
// texture cache (a texture it uploaded is uploaded; one it used stays as the picture before and the next one use it). Its resource commands (uploads, shaders ...) are the render thread's to run
// whatever happens to the replay, its draws are the pair's fresh ones (hrwFresh). Everything per-buffer that FrontPresent resets is reset here, so the next picture starts as after a picture.
void FrontFresh(uint32_t id16) {
    J3n2Flush();
    g_depthMarkNext = false; // (hrw41, c183fix F7: as FrontPresent; the buffer ends without a display copy)
    g_playerMarkDepth = 0;
    if (split::g_on) split::FinishFrame(g_buf);
    g_jobSeq = 0;
    g_lastJobKey = 0;
    g_hashedSerial = ~0u;
    const uint32_t keepPair = g_buf.hrwId | (g_buf.hrwNoFresh ? hrwpair::kPlainToken : 0u);
    g_buf.setHrwPair(0);
    g_buf.hrwReplayId = id16 & 0xFFFFu;
    g_buf.hrwFresh = true;
    g_buf.frame = g_frame;
    g_buf.presentAllowed = true;
    g.texDirty = true;
    g.texSlotDirty = 0xFF;
    if (g_frameSink) g_frameSink(g_buf);
    g_buf.clear();
    g_buf.setHrwPair(keepPair);
    g_blocksStale = true;
    for (uint32_t& o : g_blockOff) o = kNoBlock;
    g_lastDraw = -1;
    split::FrameBegin();
#if !defined(VGX_CLONE_B)
    if (vgx2_mode) two::PictureEnd();
#endif
    ListFlush();
}

void QueueSetTarget() {
    J3n2Flush(); // (GXV_J3DNATIVE=4 / 5: the register file real before it is read here)
    OcclBreak(); // (GXV_OCCLCULL: another EFB)
    SetTargetCmd t;
    t.efbW = g.efbW;
    t.efbH = g.efbH;
    g_buf.targets.push_back(t);
    g_buf.cmds.push_back(Cmd{CMD_SET_TARGET, static_cast<uint32_t>(g_buf.targets.size() - 1)});
    g_lastDraw = -1;
}

// ---- GXV_FE_TWOCORE (vgx_twocore.h): this file's per-stream state for the hand-overs, and its parts of the protocol
extern "C" int VGX2_SIDE(draw_objs)(Vgx2Obj* out) {
    int n = 0;
    out[n++] = Vgx2Obj{&cur, static_cast<uint32_t>(sizeof cur)};
    out[n++] = Vgx2Obj{&g_curKey, static_cast<uint32_t>(sizeof g_curKey)};
    out[n++] = Vgx2Obj{&g_blocksStale, static_cast<uint32_t>(sizeof g_blocksStale)};
    out[n++] = Vgx2Obj{&g_pendingPosMtxMax, static_cast<uint32_t>(sizeof g_pendingPosMtxMax)};
    out[n++] = Vgx2Obj{&g_softOkVs, static_cast<uint32_t>(sizeof g_softOkVs)};
    out[n++] = Vgx2Obj{&g_softOk, static_cast<uint32_t>(sizeof g_softOk)};
    return n;
}
#if defined(VGX_CLONE_B)
// The segment's start: no resolved state, no block known (vgx_twocore.cpp: the finishing thread deduplicates what is sent), the state
// serials from a range of their own, an empty command list.
extern "C" void vgx2_b_draw_cold(uint32_t serialBase) {
    cur = Resolved{};
    cur.stateSerial = serialBase;
    g_curKey = CurKey{};
    g_blocksStale = false;
    for (uint32_t& o : g_blockOff) o = kNoBlock;
    g_lastDraw = -1;
    g_softOkVs = 0xFFFF;
    g_cha.serial = ~0u;
    g_cha.pend = nullptr;
    g_cha.pendN = 0;
    dj::g_stale = false;
    g_buf.cmds.clear();
    g_jobSeq = 0;
}
// The picture's start: the frame and the character state the player has (B is idle).
extern "C" void vgx2_b_draw_picture(const void* cha, uint32_t chaBytes, uint64_t frame) {
    g_depthMarkNext = false; // (hrw41, c183fix F7: the second worker never runs QueueCopyDisp or FrontPresent; a picture's mark ends with it here)
    g_playerMarkDepth = 0;
    if (chaBytes == sizeof g_cha) std::memcpy(static_cast<void*>(&g_cha), cha, chaBytes);
    g_cha.pend = nullptr;
    g_cha.pendN = 0;
    g_frame = frame;
    g_traceNow = -1;
    g_buf.clear();
}
// B's command list: the placeholders' job numbers (B makes no other command: ~0u if it did).
extern "C" uint32_t vgx2_b_draw_cmds(std::vector<uint32_t>& jobs) {
    jobs.clear();
    for (const Cmd& c : g_buf.cmds) {
        if (c.type != CMD_DRAW) return ~0u;
        jobs.push_back(c.arg);
    }
    g_buf.cmds.clear();
    return static_cast<uint32_t>(jobs.size());
}
extern "C" int vgx2_b_draw_can_begin(void) { return g_cha.hudDepth == 0 && g_playerMarkDepth == 0 ? 1 : 0; }
// Whether the segment resolved a draw: only then are the resolved state and the dirty flags at its end the stream's (before its first
// resolution they are the cold start's, which the player must not take: vgx_twocore_impl.h)
extern "C" int vgx2_b_draw_resolved(void) { return cur.valid ? 1 : 0; }
// (twocore25) at a segment's end: GXV_DIRECT_J3D's deferred key copies made, so that the cur.fs / cur.vs handed over are the keys of
// cur.fsId / cur.vsId, as the player's dj::g_stale = false after the hand-over says (vgx2_a_draw_after_adopt). B's retained calls take the
// normal path (CallListRetained), so nothing is deferred in B today: this keeps it so if that changes.
extern "C" void vgx2_b_draw_sync_keys(void) { dj::SyncKeys(); }
#else
extern "C" int vgx2_reg_find(int fragment, const void* key, uint32_t n, uint64_t h) {
    if (fragment) {
        if (n != sizeof(FsKey)) return -1;
        auto range = g_fsReg.map.equal_range(h);
        for (auto it = range.first; it != range.second; ++it)
            if (std::memcmp(&it->second.key, key, n) == 0) return it->second.id;
    } else {
        if (n != sizeof(VsKey)) return -1;
        auto range = g_vsReg.map.equal_range(h);
        for (auto it = range.first; it != range.second; ++it)
            if (std::memcmp(&it->second.key, key, n) == 0) return it->second.id;
    }
    return -1;
}
extern "C" int vgx2_variant_known(uint32_t progKey, uint64_t variantKey) {
    if (!g_shaderList) return 1;
    return g_progSeen.count(progKey) && g_varSeen.count(variantKey) ? 1 : 0;
}
extern "C" void vgx2_a_cha_add(int16_t x, int16_t y, int16_t w, int16_t h) { ChaAddBox(EfbRect{x, y, w, h}); }
// (GXV_FE_TWOCORE=2) the resolved state the second worker had at a segment's end (its draw_objs, from `p`) against the player's there:
// a bit per field that differs (the state serial is not compared: its numbers are B's own by design)
extern "C" uint32_t vgx2_a_draw_compare(const uint8_t* p) {
    Resolved b;
    std::memcpy(static_cast<void*>(&b), p, sizeof b);
    const Resolved& a = cur;
    uint32_t m = 0;
    if (a.valid != b.valid) m |= 1u << 0;
    if (!a.valid || !b.valid) return m;
    // (twocore25: the player's keys as they are, not the bytes of cur.fs / cur.vs: with GXV_DIRECT_J3D a templated resolution leaves the
    // key copies undone ("keys held stale", vgx_direct.h: cur.fsId / cur.vsId are right, cur.fs / cur.vs still hold earlier keys until
    // something reads them, dj::CurFs). That was every "resolved state fields 0x6" of the console's verify runs (c49, tw6 / tw7): B, whose
    // retained calls take the normal path, holds the copied keys; the player, deferred ones. Not a difference of state.)
    if (std::memcmp(&dj::CurFs(), &b.fs, sizeof a.fs) != 0) m |= 1u << 1;
    if (std::memcmp(&dj::CurVs(), &b.vs, sizeof a.vs) != 0) m |= 1u << 2;
    if (a.fsId != b.fsId || a.vsId != b.vsId) m |= 1u << 3;
    if (std::memcmp(&a.rs, &b.rs, sizeof a.rs) != 0) m |= 1u << 4;
    if (a.texMask != b.texMask) m |= 1u << 5;
    for (int t = 0; t < 8; ++t)
        if ((a.texMask >> t) & 1) {
            if (std::memcmp(&a.tex[t], &b.tex[t], sizeof a.tex[t]) != 0) m |= 1u << 6;
            if (a.texW[t] != b.texW[t] || a.texH[t] != b.texH[t]) m |= 1u << 7;
        }
    if (a.cullAll != b.cullAll) m |= 1u << 8;
    if (a.soft != b.soft) m |= 1u << 9;
    if (a.mtxRowsUsed != b.mtxRowsUsed) m |= 1u << 10;
    if (a.foldOk != b.foldOk || a.fold != b.fold) m |= 1u << 11;
    if (a.useFs != b.useFs) m |= 1u << 12;
    if (a.palCls != b.palCls) m |= 1u << 13;
    if (a.vcdMtxMask != b.vcdMtxMask) m |= 1u << 14;
    if (a.vcdGen != b.vcdGen) m |= 1u << 15;
    return m;
}
// After the player took the second worker's state: what it derives from the resolved state is made again.
extern "C" void vgx2_a_draw_after_adopt(uint32_t jobs) {
    dj::g_stale = false;      // (GXV_DIRECT_J3D: cur's keys are the ids' keys: B made them so)
    g_cha.serial = ~0u;       // (GXV_CHARAA: the classification is made again for the new serial)
    if (jobs) g_lastDraw = 0; // (the last command is a placeholder of B's)
    g_jobSeq += jobs;
}
extern "C" void vgx2_a_draw_cha(void** p, uint32_t* n) {
    *p = &g_cha;
    *n = static_cast<uint32_t>(sizeof g_cha);
}
// Whether this picture may have segments: 0, or why not (vgx_twocore_impl.h kNoB*): nothing that inspects the moved work per draw, no
// verify mode B's instance cannot run, none of the switches whose per-draw bookkeeping B does not hand over.
// twocore24: GXV_SHADOWFILL_BOUNDS / GXV_SHADOW_RECT (ShadowFillOn, the console's default) no longer refuse. All their per-picture state is
// in the draws: a static run's job carries its alpha bounds (SfStaticBounds: the run's list boxes through the registers and the resolved
// state of the moment, which B has as the player would), its cover bit is the HUD bit only, and ShadowFillPass works from the finished
// records at the picture's end, B's among them in stream order. B makes no dynamic draw (SfDynamicBounds, the covering quads).
extern "C" int vgx2_a_draw_refusal(void) {
    if (stats::Detailed()) return 2;
    if (TraceFrame()) return 3;
    if (FeCensusOn() || PalJoinOn()) return 4;
    if (StaticInstSlots() != 0) return 5;
    if (RigidMergeLearnMax() != 0) return 6;
    if (RsCacheMode() != 1) return 7;
    if (J3nMode() != 0) return 8;      // (GXV_J3DNATIVE: its chains' state is not in the hand-over lists)
    if (HudNativeMode() != 0 || HrwMode() != 0) return 9; // (GXV_HUD_NATIVE, GXV_HRW: the HUD bit's depth, g_hudnDepth, is not either)
    return 0;
}
#endif
} // namespace vgx
// (cpu34d census: defined at namespace scope; inside the anonymous namespace it had internal linkage and vgx_dlcache.cpp could not link to it)
namespace vgx { uint8_t g_dynCenList = 0; }
