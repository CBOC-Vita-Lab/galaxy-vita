// Front-end GX state: the hardware register file (BP / XF / CP) plus the
// bookkeeping the HLE-level API adds on top (texture objects, vertex arrays).
// API calls are encoded into register writes exactly like the SDK does, and
// command streams (display lists) are parsed into the same writes, so there
// is a single source of truth for both paths.
#pragma once
#include <cstring>

#include "vgx_cmd.h"
#include "vgx_lodsampler.h"
#include "vgx_shadergen.h"
#include "vgx_stage_enum.h" // (precomp26)
#include "vgx_silhocc.h"
#include "vgx_ta27.h" // (ta27: GXV_TA27 census, pure part)

namespace vgx {
// ---- Galaxy: the game's own GX library drives the renderer through the hardware registers
// (textures, palettes, vertex arrays and EFB copies arrive as BP / CP writes with console
// addresses), so the front end asks its host to turn an address into memory and tells it
// about the pixel engine's synchronisation commands.
struct GuestHooks {
    // Console address as written into a GX register (physical, cached or uncached) -> host
    // memory, or null when nothing is mapped there. *bytesToEnd: readable bytes from there on.
    const uint8_t* (*resolve)(uint32_t gxAddr, uint32_t* bytesToEnd) = nullptr;
    void (*peFinish)() = nullptr;                          // BP 0x45 with the "done" bit
    void (*peToken)(uint16_t token, bool interrupt) = nullptr; // BP 0x47 (interrupt) / 0x48
    void (*displayCopy)() = nullptr;                       // after a display copy was queued: the frame ends here
    void (*freshEnd)(uint32_t pairId) = nullptr;           // GXV_HRW (hrw27): after PORT_GX_MARK_FRESH_END: the partial draw ends here (the host finishes the split records, then FrontFresh)
};
void SetGuestHooks(const GuestHooks& h);
const GuestHooks& Hooks();
// GXV_FIX_LODBIAS (default 0, read once): stages sample with the texture unit's LOD bias (FsKey::Stage::lodBias, vgx_fixes.h LodBiasOf). With it on, the
// texture unit's mode0 / mode1 registers feed the shader key hash and mark the keys dirty.
bool FixLodBias();
// GXV_LODBIAS_SAMPLER (default 0, read once; needs GXV_FIX_LODBIAS): 1 a stage's unit bias that the picture cannot tell from the sampler's is given to the
// sampler (TexBind::lodSteps) and the stage loses its shader bias (vgx_lodsampler.h says when); 2 count only (nothing moves, the census runs).
int LodSamplerMode();
const lodsampler::Cfg& LodSamplerCfg();
// The 8 texture units' LOD biases in 1/32 levels from a register file (vgx_fixes.h LodBiasOf x 32: 0 when a unit has none), and the hash word they make.
void LodBiases(const uint32_t* bp, int out[8]);
// GXV_FIX_TEVIN (default 0, read once): TEV inputs A, B and C read the low 8 bits of a register (FsKey::inWrap), see State::tevOor.
bool FixTevIn();
// FixTevIn(), or GXV_FS_HALF=1 / 3: State::tevOor is kept up to date (and is part of the key hash).
bool TevOorTracked();
// GXV_TEVIN_CANON=1 / 2 (vgx_shadergen.h TevInCanonMode): State::tevOor is a mask of the registers outside 0..255 (bit r = register r).
bool TevInCanonOn();
// GXV_VS_TRIM (read once, default 0): State::ltCos1 / ltDist1 are kept up to date (and are part of the key hash).
bool VsTrimTracked();
bool LtSkipOn(); // GXV_VS_LTSKIP (vertex25): the lit vertex keys carry the bit (vgx_shadergen.h VsLtSkipMode)
bool StaticCrowdEnabled();
void QueueCrowdSnapshot(const staticcrowd::Tag* tags, uint32_t count, bool valid);
void ResetCrowdTags();
void RecordCrowdCopy(CopyTexCmd& copy, uint64_t destination, uint32_t format);

struct VtxAttrFmt {
    uint8_t cnt = 1, type = 4, frac = 0; // GXCompCnt, GXCompType
};
struct ArrayRef {
    const uint8_t* data = nullptr;
    uint32_t size = 0;
    uint32_t stride = 0;
    bool le = false;
};

// Host-side layout of GXTexObj / GXTlutObj / GXLightObj. The public structs
// are opaque word arrays (96 / 48 / 64 bytes); the runtime only stores them.
struct TexObj {
    const void* data;
    uint16_t width, height;
    uint32_t format;
    uint8_t wrapS, wrapT, minFilter, magFilter;
    uint8_t mipmap, pad[3];
    float minLod, maxLod, lodBias;
    uint32_t tlut;
    void* userData;
    uint32_t dataVersion; // bumped by GXInitTexObj*/GXInitTexObjData: "contents may have changed"
    uint32_t magic;
};
struct TlutObj {
    const void* data;
    uint32_t format;
    uint16_t entries;
    uint16_t pad;
    uint32_t version;
};
struct LightObj {
    uint32_t pad[3];
    uint32_t color; // RGBA8
    float a[3], k[3], pos[3], dir[3];
};

struct TexSlot { // what GXLoadTexObj bound to a texmap
    TexObj obj{};
    bool valid = false;
    uint32_t loadSerial = 0;
    uint32_t avail = 0xFFFFFFFFu; // readable bytes at obj.data (Galaxy: to the end of the memory region it lies in)
};
struct TlutSlot {
    TlutObj obj{};
    bool valid = false;
};
constexpr int kTlutSlots = 1024;

// How one vertex format's command-stream bytes map onto the decoded layout.
// One attribute of `count` vertices: source, destination, count, source and destination strides, then what an
// indexed attribute needs (the array, its stride, the highest valid byte offset) and the scale of integer components.
// Returns non-zero when an index pointed outside its array.
using AttrFn = uint32_t (*)(const uint8_t*, uint8_t*, uint32_t, uint32_t, uint32_t, const uint8_t*, uint32_t, uint32_t, float);
struct AttrPlan {
    uint8_t attr, mode, type, cnt, frac;
    uint8_t srcSize;  // bytes consumed from the stream
    uint8_t elemSize; // bytes of one array element (indexed)
    uint8_t outOff;
    uint8_t comps;
    uint16_t srcOff;  // where in a vertex of the stream
    float scale;      // 1 / 2^frac (fn)
    AttrFn fn;        // the loop that decodes this attribute when it comes straight from the stream; null: the general path
    AttrFn fnx[2];    // the same for an attribute read through an 8- / 16-bit index, per byte order of the array (false, true); null: the general path
};
struct Plan {
    bool valid = false;
    bool direct = false;      // every attribute has an fn: DecodeVertices needs no array and no index (the fast path)
    uint8_t defaults = 0;     // bit 0: z must be zeroed (two-component position), bit 1: the matrix row (no PNMTXIDX)
    uint8_t directDefaults = 0; // defaults not supplied by the direct position loop (uses existing alignment padding)
    int n = 0;
    AttrPlan a[21];
    uint32_t streamSize = 0;
    uint32_t layoutMask = 0;
    VtxLayout layout;
    int callsPerVertex = 0; // immediate mode
    // Fixed four-vertex decode selected from the actual attribute descriptors, never stream data.
    bool (*quad)(const Plan&, const uint8_t*, uint8_t*) = nullptr;
};
// (`f`: the same plans laid out with both colour slots, for register folding: see FoldGuess)
struct PlanCache { Plan p[8]; Plan f[8]; };
// The plan with the colour slots a folding draw needs (FoldGuess says when to ask), cached with the plan it comes from.
const Plan& GetFoldedPlanIn(PlanCache& cache, int fmt, const Plan& base);
const Plan& FoldedPlan(int fmt, const Plan& base); // (the player's cache)
// Plans are cached per vertex format until the descriptor / formats change
// (`dirty`). `recordStats` must only be set on the player's thread.
const Plan& GetPlanIn(PlanCache& cache, bool& dirty, const uint8_t* vcd, const VtxAttrFmt (*vat)[26], int fmt, bool recordStats);

struct State {
    // ---- BP
    uint32_t bp[256] = {};
    uint32_t bpMask = 0x00FFFFFF;
    int tevReg[4][4] = {};  // PREV, REG0..2 as r,g,b,a (s11)
    int tevKonst[4][4] = {};
    // ---- XF
    float xfPos[64][4] = {};  // 0x0000..0x00FF
    float xfNrm[32][3] = {};  // 0x0400..0x045F
    float xfPost[64][4] = {}; // 0x0500..0x05FF
    uint32_t xfLight[8][16] = {}; // 0x0600..0x067F
    uint32_t xfReg[0x58] = {};    // 0x1000..0x1057
    // ---- CP
    uint8_t vcd[26] = {};
    VtxAttrFmt vat[8][26] = {};
    ArrayRef arrays[26] = {};
    uint32_t cpBase[16] = {}, cpStride[16] = {}; // CP 0xA0+n / 0xB0+n as written (Galaxy)
    uint16_t cpArrayStale = 0xFFFF;              // of those: not yet turned into g.arrays[] (BpArraySync)
    uint8_t bpTexStale = 0xFF;                   // texmaps whose g.tex[] is older than their BP registers (BpTexSlotSync)
    uint32_t matIdxA = 0x3CF3CF00u | 0, matIdxB = 0x00F3CF3Cu; // identity texture matrices, PNMTX0
    // ---- API-level state
    TexSlot tex[8];
    TlutSlot tlut[kTlutSlots]; // Galaxy: one per 512 bytes of palette memory (BP 0x65), see vgx_bpsync.cpp
    uint16_t manualScale[8][2] = {};
    uint8_t manualScaleOn = 0;
    EfbRect dispCopySrc{0, 0, 640, 480};
    EfbRect texCopySrc{0, 0, 640, 480};
    uint16_t dispCopyDstW = 640, dispCopyDstH = 480;
    uint16_t texCopyDstW = 0, texCopyDstH = 0;
    uint32_t texCopyFmt = 6;
    bool texCopyHalf = false;
    uint16_t efbW = 640, efbH = 528; // logical EFB size (VIConfigure)
    // ---- dirty tracking (coarse: gates rebuilding keys / blocks, which are
    // then compared against the previous ones before anything is emitted)
    // (one run of bytes: DirtyAny tests them all with three loads)
    struct {
        bool bpDirty = true;      // a BP register the TEV uniform block is built from
        bool bpKeyDirty = true;   // a BP register the fragment key is built from (then the render state and the TEV block are rebuilt too)
        bool bpRsDirty = true;    // a BP register only the render state is built from (scissor, depth mode, line width)
        bool xfRegDirty = true;   // an XF register not covered by the flags below: everything
        bool xfKeyDirty = true;   // channel control, texgens: vertex / fragment key
        bool xfViewDirty = true;  // viewport: render state
        bool xfProjDirty = true;  // projection
        bool xfColorDirty = true; // ambient / material colours: light block
        bool xfMtxDirty = true;   // matrix indices, normal and post-transform matrix memory (position / texture matrix memory: xfPosRowsDirty)
        bool xfLightDirty = true; // light memory
        bool texDirty = true;     // texture bindings
        bool dirtyPad = false;    // (never set: the run is twelve bytes)
    };
    bool DirtyAny() const {
        uint32_t w[3];
        std::memcpy(w, &bpDirty, 12);
        return (w[0] | w[1] | w[2]) != 0;
    }
    // Rows of the position / texture matrix memory written since the last draw. A draw only
    // rebuilds its matrix blocks when a row it uses is among them (a particle loads its own
    // position matrix, which the soft transform consumes without any block).
    uint64_t xfPosRowsDirty = ~0ull;
    // TEV colour registers (bits 0..3: PREV, REG0, REG1, REG2) and konst colours (bits 4..7)
    // written since the last draw: the TEV block's values, apart from bpDirty.
    uint8_t tevColDirty = 0xFF;
    // GXV_FIX_TEVIN (and GXV_FS_HALF=1 / 3: TevOorTracked): 1 while a TEV colour register (PREV, REG0..2, any component) is outside 0..255; part of the shader key hash (FsKey::inWrap).
    // GXV_TEVIN_CANON (TevInCanonOn): a mask instead, bit r = register r (0 PREV .. 3 REG2) has a component outside 0..255.
    uint8_t tevOor = 0;
    // GXV_VS_TRIM (VsTrimTracked): bit l = light l's cos attenuation (XF light words 4..6) is exactly (1, 0, 0) / its distance attenuation
    // (words 7..9) is; kept by WriteXFWord, part of the key hash (VsKey::ltCos1 / ltDist1). 0 with the switch off.
    uint8_t ltCos1 = 0, ltDist1 = 0;
    bool layoutDirty = true;  // VCD / VAT
    uint32_t vcdGen = 0;      // counts the changes of the vertex descriptor (FoldGuess)
    uint8_t texSlotDirty = 0xFF; // which texmaps' bindings (or what they resolve to) may have changed
    // ---- no-op tests of register writes (GXV_REGSHADOW, vgx_state.cpp)
    // BP 0xE0..0xE7 hold the TEV colour registers and the konst colours at the same addresses (bit 23 tells them apart), and
    // every J3D material writes both, so bp[] always holds the other kind and a write never looked like a repeat. These are the
    // halves as last written, by (register - 0xE0) * 2 + bit 23 (0: not known); tevReg / tevKonst always hold what they say.
    uint32_t tevRaw[16] = {};
    // CP 0x50 / 0x60 (vertex descriptor) and 0x70..0x97 (vertex formats) as last written (bit n of cpRawValid: slot n known;
    // see CpRawSlot). Each of them sets its own fields of vcd[] / vat[] completely, so writing it again changes nothing.
    uint32_t cpRaw[26] = {};
    uint32_t cpRawValid = 0;
    // Hash of the registers the shader keys are built from (see KeyHashOf in vgx_state.cpp; maintained unless GXV_RSCACHE=0).
    uint64_t keyHash = 0;
};
extern State g;

// ---- register writes (vgx_state.cpp)
void WriteBP(uint32_t regAndValue);            // bits 24..31 = register
inline void WriteBP(uint8_t reg, uint32_t v) { WriteBP((static_cast<uint32_t>(reg) << 24) | (v & 0x00FFFFFFu)); }
void SetBPField(uint8_t reg, int shift, int count, uint32_t value); // read-modify-write helper for the API
void WriteXF(uint32_t addr, uint32_t count, const uint32_t* hostWords);
void WriteXFBE(uint32_t addr, uint32_t count, const uint8_t* beWords);
inline void WriteXFReg(uint32_t addr, uint32_t v) { WriteXF(addr, 1, &v); }
void WriteCP(uint8_t reg, uint32_t value);
void SetVcd(int attr, int type);
void SetVat(int fmt, int attr, int cnt, int type, int frac);
void StoreBP(uint8_t reg, uint32_t value);      // final register value (the recorder applied the mask already)
void InitState(State& s);                      // power-on values
// GXV_RSCACHE (vgx_state.cpp, vgx_draw.cpp): 0 off, 1 on, 2 verify. Read once.
extern int g_rsCacheMode;
int RsCacheModeSlow();
inline int RsCacheMode() { return __builtin_expect(g_rsCacheMode >= 0, 1) ? g_rsCacheMode : RsCacheModeSlow(); }
uint64_t KeyHashOf(const State& s); // State::keyHash computed from scratch
void ResetState();

// ---- geometry (vgx_state.cpp)
// Returns the bytes consumed: everything up to the last complete command (a command cut off
// by the end of the buffer is left for the caller to present again with the rest of it).
// inList: the bytes are a display list (a CALL_DL inside it is ignored, as on the console).
uint32_t ProcessStream(const uint8_t* data, uint32_t nbytes, bool inList = true);
// A display list of state commands only, from its pre-parsed form (vgx_state.cpp); false: not handled, use ProcessStream.
bool StateListCall(const uint8_t* list, uint32_t nbytes);
// (GXV_FE_CENSUS, fedl25) why the last StateListCall of this thread returned false, and for an opcode the replay does not make, which one:
// 0 it did not (a replay was made), 1 GXV_STATELIST=0, 2 not guest RAM, 3 quiet after a refusal (opcode in cmd: the list holds a draw / a call / an
// unknown opcode), 4 quiet after churn (rewritten with other bytes on most calls), 5 refused now (opcode in cmd), 6 churn now, 7 raced with a write
uint8_t StateListWhyLast(uint8_t* cmd);
bool StateListFastHit(const uint8_t* list, uint32_t nbytes);  // (CallList's shortcut for the common case)
// (feplan31) GXV_FEPLAN_CENSUS: a list call that ends in a full parse, timed and counted by the way it got there (path: vgx_state.cpp FpTiny ..
// FpOff: 0 tiny, 1 not guest RAM, 2 bypassed after Build refused it, 3 bypassed for rewrites / a variant streak, 4 Build refused it now,
// 5 the bypass set now, 6 cache off / traced picture) and by what it holds; FePlanCensusParse makes the ProcessStream itself.
bool FePlanCensusOn();
void FePlanCensusParse(const uint8_t* list, uint32_t nbytes, uint8_t path);
// GXV_TINYLIST / GXV_TINYLIST_CENSUS (tinylist31, vgx_tinylist.inc): tiny lists that begin with a state command (J3D's small differed lists)
// through the state-list cache; the census of CallListBody's tiny branch. The branch's ways (the census's "by way"):
enum : uint8_t { kTwParsed, kTwPadReplay, kTwPadParsed, kTwTlReplay, kTwTlParsed, kTwTlVerified, kTwWays };
extern uint8_t g_tlSite; // (census) the call site of the list called now: 0 CALL_DL, 1 J3D shape call (0x59), 2 particle command item (0x58), 3 j3n bracket
int TinyListMode();      // GXV_TINYLIST: 0 off (default), 1 on, 2 verify (diagnostics builds; else 0)
inline bool TinyListCandidate(uint8_t first) { // (the list's first byte is a state command: XF, CP, BP, an indexed matrix load)
    return first == 0x10 || first == 0x08 || first == 0x61 || first == 0x20 || first == 0x28 || first == 0x30 || first == 0x38;
}
bool TinyListCall(const uint8_t* list, uint32_t nbytes, int mode); // true: done (replayed; =2: parsed and checked); false: parse it
bool TinyListCensusOn();
void TinyListCensusCall(const uint8_t* list, uint32_t nbytes, uint8_t (*branch)(const uint8_t*, uint32_t));
// fast-hit replays, StateListCall replays, refused, backed off (one look), =2: checked, mismatching, not compared (trigger), not replayed
void TinyListCounters(uint64_t out[8]);
// GXV_FEPLAN (mixed display lists' plans): replays, plans made, refused (vertex format written before a draw), replays that found a draw's
// vertex size changed; =2: checked, mismatching, not compared (size changed), not compared (a trigger in the list)
void FePlanCounters(uint64_t out[8]);
void StateListCounters(uint64_t out[5]); // hits, builds, refused, verify checks, verify mismatches
// heap23b (GXV_SL_SCENEDROP): the state-list store at a scene's destruction (front-end thread, before the bytes published after it).
// Its entries are keyed by list address and were only ever freed all at once at 16,384 entries, so the lists of every scene visited
// stayed (each entry with copies of its list: bytes, operations, words, deltas). mode 0: only counted; 1: the entries of lists in
// [lo[i], hi[i]) (the destroyed scene's heaps) are freed; 2: every entry is freed. A freed entry is parsed again at its next call,
// as after the store's own clear: the register state each call leaves is the same.
struct StateListStore {
    uint32_t entries = 0, dropped = 0;           // entries left in the store, entries freed now
    uint64_t bytes = 0, droppedBytes = 0;        // their host bytes (the entry and its four vectors' capacities)
    uint64_t clears = 0;                         // the store's own clears at 16,384 entries so far
};
StateListStore StateListsSceneGone(const void* const* lo, const void* const* hi, int ranges, int mode);
// (the "fe cuts" workload line) material lists replayed by StateListFastHit; of the delta replays, those in the compact layout
// (GXV_FECUT_SLHOT); two words unused
void FeCutCounters(uint64_t out[4]);
void SlDeltaCounters(uint64_t out[5]); // (GXV_SLDELTA) replays by the register deltas; =2: compared with SlReplay, mismatching; lists compiled, refused
void StateListFastCounters(uint64_t out[3]); // (GXV_SLFAST) StateListFastHit's replays found by the front, by the table; of them, rewrites with the same bytes
// (GXV_DRAWREPLAY, vgx_dlcache.cpp) calls replayed from the previous picture's trace, records found that failed a test, records found away from
// the cursor, hits recorded by the normal path; (5) and the list calls the trace saw
void DrawReplayCounters(uint64_t out[4]);
void DrawReplayCounters5(uint64_t out[5]);
// The fecache switches' counters, for the host's status line (totals since start):
void RegShadowCounters(uint64_t out[2]);      // GXV_REGSHADOW=2: writes checked, mismatching
void NoopFilterCounters(uint64_t out[2]);        // GXV_NOOP_FILTER=2: commands checked, mismatching
void StateFastCounters(uint64_t out[4]);        // GXV_FE_STATEFAST: [0] repeats checked (=2), [1] mismatching, [2] commands the loop took, [3] runs
void RigidMergeCounters(uint64_t out[2]);     // GXV_RIGIDMERGE: palette runs made, draws that joined one (since the start)
void ResolveCacheCounters(uint64_t out[4]);   // GXV_RSCACHE: draws checked (=2), mismatching, key memo hits, misses
void TexMemoCounters(uint64_t out[3]);        // GXV_TEXMEMO: memo hits, checked (=2), mismatching
// ---- register-driven textures, palettes and copies (vgx_bpsync.cpp)
void BpTexSlotSync(int texMap);          // rebuilds g.tex[texMap] from BP 0x80.. / 0xA0..
void BpTlutLoad();                       // BP 0x65 written: copy the palette named by 0x64 / 0x65 into palette memory
void BpCopyTrigger(uint32_t cmd);        // BP 0x52 written: EFB copy to a texture or to the display
bool BpZTexConstant(float& z);           // the Z texture of a GX_ZT_REPLACE draw, when it is one constant value
void BpArraySync(int cpIndex);           // g.arrays[] from CP 0xA0+n / 0xB0+n
void SyncVertexArraysSlow();
inline void SyncVertexArrays() { if (g.cpArrayStale & 0x0FFF) SyncVertexArraysSlow(); } // before anything reads g.arrays[POS..TEX7]
void CallList(const uint8_t* list, uint32_t nbytes); // a display list in guest RAM: through the geometry cache (vgx_dlcache.cpp)
// GXV_RETAINED (vgx_dlcache.cpp "retained shapes"): the game's J3D shape draws come as a port-private handle (opcode 0x59, a u32)
// right in front of their CALL_DL. CallListRetained is CallList for such a call: by the handle straight to the cache entry the
// shape's last call was a hit of, checked by the CP words of the vertex state and the generations kept with it; anything else: CallList.
constexpr uint8_t kRetainedOp = 0x59;
bool RetainedOn();                                                          // GXV_RETAINED >= 1 (read once)
void CallListRetained(const uint8_t* list, uint32_t nbytes, uint32_t handle);
// calls, hits, no record for the handle, a record that failed a test, recorded, (=2) checked, (=2) mismatching
void RetainedCounters(uint64_t out[7]);
bool CallListIsTinyStream(const uint8_t* list, uint32_t nbytes); // (GXV_FXMERGE) CallList would parse it at once with ProcessStream
// ---- GXV_J3DNATIVE (j3n1: vgx_j3native.inc, included by vgx_state.cpp; decomp/src/Game/System/DrawBuffer.cpp, decomp/src/RVL_SDK/gx/GXDisplayList.c)
// The game brackets each J3D material-list application and each differed-list call with a skippable marker pair carrying an id and
// versions (opcode 0x5A, five u32: id, versionA, versionB, ext, flags; then the call's own 0x40 addr size, then 0x5B), in the top-level
// stream only. =0 (default): nothing is sent, and a marker found anyway is only consumed. =1 census (read-only, the pictures are the
// normal path's); =2 verify / probation (every bracket that could be skipped is also applied the normal way and the register files are
// compared); =3 on (a bracket whose record is trusted skips the list's lookup and validation and replays the recorded delta);
// =4 stage 2 verify; =5 stage 2 on (see below and vgx_j3native2.inc).
constexpr uint8_t kJ3nOp = 0x5A, kJ3nEndOp = 0x5B;
extern int g_j3nMode;
int J3nModeSlow();
inline int J3nMode() { return __builtin_expect(g_j3nMode >= 0, 1) ? g_j3nMode : J3nModeSlow(); }
inline bool J3nCensus() { const int m = J3nMode(); return m >= 1 && m <= 3; } // (the stage 0 / 1 hooks of the top-level stream)
// GXV_J3DNATIVE=4 (stage 2 verify) / 5 (stage 2 on): vgx_j3native2.inc (chains of recorded list calls replayed as register patches),
// vgx_j3native2_draw.inc (templated static runs, GXV_J3DNATIVE_TPL=1 only), vgx_j3n2.h. The register file stays real; J3n2Flush ends an
// active chain before anything outside the stream (copies, targets, the picture's end) runs. Exact, measured not to pay: keep 0.
void J3n2Flush();
void J3n2SceneGone();
void J3n2PictureEnd();
void J3n2Counters(uint64_t out[24]);                   // (the fixture's line)
void J3n2Bracket(const uint8_t* list, uint32_t nbytes, uint32_t id, uint32_t vA, uint32_t vB, uint32_t ext, uint32_t fl);
// vgx_dlcache.cpp: CallListRetained's record-hit branch alone (true: done; false: nothing done, the caller makes the normal call).
bool CallListRetainedHit(const uint8_t* list, uint32_t nbytes, uint32_t handle);
void J3nPictureEnd();                                  // vgx_draw.cpp, at the end of every picture (front-end thread)
namespace sc { void PictureEnd(); }                    // GXV_STREAM_CENSUS (vgx_census.inc): the counted picture ends at its display copy (QueueCopyDisp)
void J3nSceneGone();                                   // vgx_state.cpp: the state-list store was dropped (records die with it)
void J3nCounters(uint64_t out[24]);                    // (the fixture's line)
bool J3nListPathOk(uint32_t nbytes);                   // vgx_dlcache.cpp: CallList would take CallListBody's state-list fast hit for such a call
// (GXV_FXMERGE, vgx_state.cpp FxMergeRun) commands, items, list calls, calls that went the short way, of them with the decode reused,
// calls that went through CallList
void FxMergeCounters(uint64_t out[6]);
// ---- GXV_FE_PREFETCH (look-ahead cache hints; see "look-ahead" before ProcessStream in vgx_state.cpp)
// A data-cache hint (PLD on ARM): never faults, never changes a value; only when the line arrives.
#if defined(GXV_CACHE24_MODEL)
// Host cache model only (tests, cachegrind): valgrind's cache simulation ignores PLD, so a hint is modelled as a read that fills the lines
// (GXV_CACHE24_MODEL=1: only the cache24 hints Pf24; =2: the older hints FePrefetch as well). No timing: a hinted line is a hit later.
__attribute__((noinline)) inline void gxv_model_touch(const void* p) { volatile const char c = *static_cast<const volatile char*>(p); (void)c; }
#endif
#if defined(GXV_CACHE24_MODEL) && GXV_CACHE24_MODEL >= 2
inline void FePrefetch(const void* p) { gxv_model_touch(p); }
#else
inline void FePrefetch(const void* p) { __builtin_prefetch(p, 0, 3); }
#endif
// The first `maxLines` 32-byte lines (the console's L1 / L2 line) of [p, p + bytes).
inline void FePrefetchLines(const void* p, uint32_t bytes, uint32_t maxLines) {
    if (!p || !bytes) return;
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    uint32_t n = static_cast<uint32_t>(((a & 31u) + bytes + 31u) >> 5);
    if (n > maxLines) n = maxLines;
    const uint8_t* q = reinterpret_cast<const uint8_t*>(a & ~static_cast<uintptr_t>(31));
    for (uint32_t i = 0; i < n; ++i) FePrefetch(q + i * 32u);
}
// GXV_FECUT_HINT (read once; default 0: off): cache hints for what a cached call reads next, issued from what the call itself has just
// loaded, never ahead of it and never by loading anything more: a material hit hints its delta (all of it, at most 24 lines) as soon as
// its entry's first line is read, before it looks up the generation counter; a same-bytes rewrite hints the first lines of the list and of
// the copy it compares; a replayed geometry hit hints its runs, its bounds and its list's generation counter before it compares the vertex
// state. The delta, the runs and the bounds are separate blocks of memory the game's turn has pushed out of the caches: demand loads
// fetch them one line at a time as the loop reaches them, the hints let several lines arrive at once. Values never change (PLD never
// faults and is never read); only the time the loads take. (GXV_FE_PREFETCH, which lost on the console, parsed the stream ahead and
// loaded the tables of calls up to four ahead to find their entries: those loads missed themselves; these hints cost 2-30 instructions.)
inline bool FeCutHint() { return VGX_KNOB(EnvInt("GXV_FECUT_HINT", 0) != 0) != 0; }
// ---- GXV_CACHE24 (cache24, 10 Nov 2026): fewer last-level cache misses on the per-draw paths; every change exact (hints and layout only).
//   bit 1 (GXV_CACHE24=1): software prefetch along the known future. The front end remembers, per retained shape record, the record of the shape
//         called after it last picture (RtSlot::nx1) and, in the draw-replay trace, the list calls the last picture made in order; at a call it
//         hints the lines the calls 1..3 ahead will read, each stage from lines the stage before it hinted a call earlier (so that no hint
//         waits on a load that misses: a stage reads only lines that were hinted a whole call ago). The back end hints the program and texture
//         records of the next draw record. A hint (PLD) never faults and never changes a value.
//   GXV_CACHE24_DIST=<n> (1..3, default 1): the distance, in calls, of the last stage (the earlier stages are 1 and 2 calls before it).
//   GXV_CACHE24 is read once; default 0 (off) until the console A/B.
inline int Cache24() { return static_cast<int>(VGX_KNOB(static_cast<int>(EnvInt("GXV_CACHE24", 0)))); }
inline bool Cache24Pf() { return (Cache24() & 1) != 0; }
inline uint32_t Cache24Dist() {
    return static_cast<uint32_t>(VGX_KNOB([] { const long v = EnvInt("GXV_CACHE24_DIST", 1); return static_cast<int>(v < 1 ? 1 : v > 3 ? 3 : v); }()));
}
inline uint64_t g_c24Pf = 0; // (front end player: hints issued, since the start)
inline void Pf24(const void* p) {
    ++g_c24Pf;
#if defined(GXV_CACHE24_MODEL)
    gxv_model_touch(p);
#else
    __builtin_prefetch(p, 0, 3);
#endif
}
// The first `maxLines` 32-byte lines of [p, p + bytes).
inline void Pf24Lines(const void* p, uint32_t bytes, uint32_t maxLines) {
    if (!p || !bytes) return;
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    uint32_t n = static_cast<uint32_t>(((a & 31u) + bytes + 31u) >> 5);
    if (n > maxLines) n = maxLines;
    const uint8_t* q = reinterpret_cast<const uint8_t*>(a & ~static_cast<uintptr_t>(31));
    g_c24Pf += n;
    for (uint32_t i = 0; i < n; ++i) {
#if defined(GXV_CACHE24_MODEL)
        gxv_model_touch(q + i * 32u);
#else
        __builtin_prefetch(q + i * 32u, 0, 3);
#endif
    }
}
// GXV_CACHE24_DELTA=<n> (1..24, default 16): the most lines of a material list's compiled delta hinted at once (the Cortex-A9 keeps four
// line fills in flight: a longer burst may be dropped in part; the console A/B decides).
inline uint32_t Cache24DeltaLines() {
    return static_cast<uint32_t>(VGX_KNOB([] { const long v = EnvInt("GXV_CACHE24_DELTA", 16); return static_cast<int>(v < 1 ? 1 : v > 24 ? 24 : v); }()));
}
void StateListAhead(const uint8_t* list, int stage); // (cache24; vgx_state.cpp) what a state-list call of `list` reads, hinted in stages 1..3
// (cache24 counters, player thread only) retained calls that had a predecessor, of them the successor was predicted right; trace look-aheads made,
// their records that were state-list calls / cached geometry
struct C24Stats { uint64_t rtCalls = 0, rtRight = 0, trAhead = 0, trSl = 0, trGeo = 0; };
inline C24Stats g_c24s;
int FePrefetchDepth(); // GXV_FE_PREFETCH (read once): how many CALL_DL ahead the top-level stream is looked at (0: off)
// What CallList of `list` will read, hinted in four stages as the call comes nearer (stage 1: four calls ahead ... 4: the next one).
// Each stage reads only what is the player's own and valid (front tables, entries) and issues hints; a stage may run without the
// earlier ones. (vgx_dlcache.cpp; the state-list part in vgx_state.cpp, the generation counters in vgx_guestmem.cpp)
void CallListPrefetch(const uint8_t* list, uint32_t nbytes, int stage);
void StateListPrefetch(const uint8_t* list, uint32_t nbytes, int stage);
// The shader-key memo set (then the keys) and the texture-binding memo slots (then their entries) that the next draw's resolution
// reads, for the registers as they are now (vgx_draw.cpp, vgx_texcache.cpp). Called before each top-level CALL_DL.
void ResolvePrefetch();
void TexMemoPrefetch(uint8_t texMaps, int step);
void FePrefetchCounters(uint64_t out[2]); // (since the start) top-level CALL_DLs; of them, those whose next call was hinted one call ahead
void VarFrontCounters(uint64_t out[2]);   // GXV_VARFRONT (vgx_draw.cpp NoteVariant): variants found in front of the seen-sets, not found
void DlCacheReport(FILE* f);
void DlBuildCounters(uint64_t out[2]); // (endwait26, GXV_ENDWAIT_LOG) display-list builds so far: time in ns, count (vgx_dlcache.cpp; zero with the switch off)
uint64_t DlCacheLiveBytes();   // decoded vertices the front end keeps for cached lists (a count the player thread updates; readable elsewhere for logs)
uint64_t DlCacheStaticBytes(); // bytes of the static buffers the back end holds for promoted lists
void DlCacheListCounts(size_t& lists, size_t& bypassed); // (heap23b) lists with cache entries, lists in the bypass table (front-end thread)
void DrawStream(uint8_t prim, int fmt, const uint8_t* data, uint32_t count, uint32_t availBytes);
// Byte ranges of the vertex arrays (attributes POS..TEX7) a decode read.
struct ArraySpans {
    uint32_t mask = 0; // bit per attribute number
    uint32_t lo[12] = {}, hi[12] = {};
};
const Plan& CurrentPlan(int fmt); // the player's plan for a vertex format under the current descriptor
ArraySpans MeasureArraySpans(const Plan& p, const uint8_t* src, uint32_t count, bool* complete = nullptr);
void DecodeVertices(const Plan& p, const uint8_t* src, uint32_t count, uint8_t* out, ArraySpans* spans);
uint32_t StreamVertexSize(int fmt);
void DrawDecoded(uint8_t prim, uint32_t layoutMask, const uint8_t* verts, uint32_t count); // vertices already in the decoded layout

// ---- draw resolution and command buffer (vgx_draw.cpp)
// Reserves room for decoded vertices; EmitDraw turns them into a DrawRec with
// the current state. Vertex data is written between the two calls.
uint8_t* BeginVertices(uint32_t layoutMask, uint32_t count);
// `foldedFrom`: the vertices were decoded into the layout FoldGuess chose (`layoutMask`, with both colour slots)
// instead of the stream's own (`foldedFrom`). When the draw turns out not to fold, nothing is drawn and the
// arena is as it was before BeginVertices; the result is false then, and the caller decodes again, plainly.
// `copyFrom` (GXV_SPLIT_COPY): the vertices are not written yet but are a copy of these bytes (split::CopiesVertices), which stay as
// they are until split::Settle(split::Issued()) after the call returns.
bool EmitDraw(uint8_t gxPrim, uint32_t layoutMask, uint32_t count, uint32_t foldedFrom = 0xFFFFFFFFu, const uint8_t* copyFrom = nullptr);
// (GXV_DYN_CENSUS, cpu34d: 1 while the draw being emitted is a cached display list's segment replayed from the frame arena - a J3D shape
// under the static-buffer limits, any other small cached list - and 0 for one decoded from the stream: a particle's quad, 2D, immediate
// geometry. Written by vgx_dlcache.cpp around its EmitDraw; read by the census only.)
extern uint8_t g_dynCenList;
// (streamc31, GXV_DYN_CENSUS bit 2: the stream-decode census, vgx_deccen.h / vgx_state.cpp DecCenNote.) DecCenNs: the front end's clock for the
// census's timed scopes (vgx_draw.cpp, FeClock: the one place of the front end that names the clock). DecCenPicture: a picture ends (FrontPresent).
// DecCenLog: the census's lines for the window, each handed to `emit`.
uint64_t DecCenNs();
void DecCenPicture();
void DecCenLog(void (*emit)(const char* line));
// The layout to decode the next draw's vertices into: with both colour slots when the last draws folded their
// registers (a particle run) and the next is small enough to, so that EmitDraw need not lay them out again.
// `layoutMask` itself when no guess is made.
uint32_t FoldGuess(uint32_t layoutMask, uint32_t count);
// A primitive whose vertices and indices already sit in a static buffer.
// vtxFirst: what the buffer's indices for this primitive were offset by (see "vertex addressing" in vgx_draw.cpp).
void EmitStaticDraw(uint8_t gxPrim, uint32_t layoutMask, uint32_t count, uint32_t bufId, uint32_t vtxByteOffset, uint32_t idxOffset,
                    uint32_t vtxFirst);
// A run of primitives of one static buffer as one record: `prims` primitives of one kind and
// layout, vtxCount vertices, idxCount indices from idxOffset on (vgx_dlcache.cpp: Run).
void EmitStaticRun(uint8_t primKind, uint32_t layoutMask, uint32_t vtxCount, uint32_t idxCount, uint32_t prims, uint32_t bufId, uint32_t vtxByteOffset,
                   uint32_t idxOffset, uint32_t vtxFirst);
bool AbsoluteIndices(); // indices count from the start of a buffer (default) / of the draw (GXV_ABSIDX=0)
// Before EmitDraw / EmitStaticDraw: the highest position-matrix row the primitive's vertices
// name (0 when they carry no matrix index). Not said: the full matrix palette is used.
void SetPendingPosMtxMax(uint32_t row);
// GXV_DLVCACHE (ta23, vgx_dlcache.cpp): the next EmitStaticRun may draw `count` indices at `idxOffset` (the run's triangles reordered for
// the vertex cache) instead of its own, if its render state makes the order of its triangles invisible. Consumed by that call.
void SetPendingIdxAlt(uint32_t idxOffset, uint32_t count);
// GXV_FACECULL (farlod32; vgx_facecull.h, vgx_draw.cpp "view lists"): 0 off (default), 1 on, 2 census (the lists are worked out and counted,
// every run is drawn whole, nothing is uploaded), 3 test picture (only the triangles a list leaves out are drawn: the runs must vanish).
// SetPendingFace: the next EmitStaticRun's run has this view-list record (consumed by that call). The Dl* services are the cache's side.
namespace facecull { struct Run; }
int FaceCullMode();
void SetPendingFace(facecull::Run* r);
void DlFaceInstall(facecull::Run& r, std::vector<uint8_t>& vtx, std::vector<uint16_t>& idx, const float eye[3], float margin, int side);
void DlFaceDrop(facecull::Run& r);
void DlFacePump(uint64_t frame);
void DlFaceTotals(uint64_t out[6]);
uint32_t DlFaceMinIdx(); // (GXV_FACECULL_MIN)
// GXV_VTXPACK (vgx_dlcache.cpp "packed static runs"): the next EmitStaticRun's run is packed with these shifts (5 bits each: position,
// normal, tex0, tex1); consumed by that call. VtxPackOn: the switch (read once). VtxPackCounters (any thread, relaxed): lists packed,
// runs packed / looked at, their vertices, static-buffer bytes before / after; VtxPackDrawCounters: packed records made, their
// matrix blocks scaled, scalings that met a matrix element too small to scale exactly (expected 0), keys the scaling cannot follow
// (expected 0: such runs are never packed).
void SetPendingVq(uint32_t vqK);
bool VtxPackOn();
void VtxPackCounters(uint64_t out[7]);
void VtxPackDrawCounters(uint64_t out[4]);
// (vgx_dlcache.cpp) GXV_DLVCACHE totals: runs reordered, their triangles, modelled vertex-shader runs unwelded / welded / reordered,
// runs left in the list's order by the budget
void DlVcacheCounters(uint64_t out[6]);
void VcacheDrawCounters(uint64_t out[4]); // (tests) static runs that took the reordered block / that had one; their indices
void NoteBypassStream(uint32_t nbytes); // (vgx_dlcache.cpp: a bypassed list is about to be parsed by ProcessStream)
void StreamDetailCounters(uint64_t out[13]);
uint32_t LastDecodedPosMtxMax(); // of the last DecodeVertices call
// Number of indices a GX primitive of `count` vertices turns into; writes them (relative to `base`) when out != null.
uint32_t PrimIndices(uint8_t gxPrim, uint32_t count, uint32_t base, uint16_t* out);
uint32_t RigidMergeListMax(); // GXV_RIGIDMERGE_DLMAX: a rigid cached list of at most this many vertices stays in the frame's arena (0: none)
uint32_t RigidMergeLearnMax(); // GXV_RIGIDMERGE_DLLEARN: a rigid cached list of at most this many vertices is copied into the arena where that joined a run (0: off)
// GXV_RIGIDMERGE_DLLEARN (vgx_dlcache.cpp, vgx_draw.cpp): jobs (draws handed to the record stage) this frame, and the shader key of the last one; the next
// static draw is one whose list can also be copied into the arena; at the frame's end, the join log of the jobs (FinishCtx::joinLog) is handed to the cache.
uint32_t DlJobSeq();
uint32_t DlLastJobKey();
void SetPendingStaticCopyable(bool on);
// GXV_STATIC_INST (fe23c; vgx_draw.cpp "static instances", vgx_dlcache.cpp "instance buffers"): consecutive uses of one cached list's
// static run that differ only in their model-view matrix are drawn as one record, from a copy of the run replicated K times (copy k's
// position w = 3k, the palette slot) with the matrix palette of GXV_RIGIDMERGE. StaticInstSlots(): K (10 or 4) when on, else 0.
// SetPendingStaticInst(id): the next static runs' list has the instance buffer `id` (0: none). StaticInstCounters: runs made, draws
// that joined one, joins refused at the record stage (a run of instances full or broken); DlStaticInstCounters (the cache): lists with
// an instance buffer now, their bytes, rebuilds asked for, lists refused, instance buffers made in all.
uint32_t StaticInstSlots();
void SetPendingStaticInst(uint32_t bufId);
void StaticInstCounters(uint64_t out[3]);
void DlStaticInstCounters(uint64_t out[5]);
// (GXV_DL_DEDUP, bowl32) shareable static buffers made, entries that took a live buffer, census would-hits, instance buffers shared, buffers now shared, live static buffers
void DlDedupCounters(uint64_t out[8]);
void DlLearnFrameEnd(const std::vector<uint8_t>& joinLog, uint64_t frame);
// copies made, bytes copied, copies that joined, copies that did not, static uses, static uses that could have joined, bytes of vertices kept, lists kept
void DlLearnCounters(uint64_t out[8]);
uint32_t SoftXformLimit(); // primitives up to this many vertices may be transformed on the CPU (and merged)
// GXV_HASH=2 needs the bytes of a static buffer to hash what a draw refers to (vgx_dlcache.cpp).
bool StaticBufferData(uint32_t bufId, const uint8_t*& vtx, const uint16_t*& idx);
void QueueCopyTex(const void* dest, bool clear);
void QueueCopyDisp(bool clear);
// GXV_SHADOWFILL_BOUNDS (vgx_draw.cpp, "shadow fill bounds"): on (read once), and (tests) totals since the start: silhouette fills
// left out, scissored, left whole; alpha captures made a fill of 0, and of those the ones not made at all (the texture held 0).
bool ShadowFillOn();
bool SfBoundsOn(); // (fix47) ShadowFillOn() or GXV_CORNERPASS: the alpha bounds and N are worked out (no cut is made for GXV_CORNERPASS alone)
// GXV_SHADOW_CULLCOVER (gpuab23, default 1 while ShadowFillOn): a full rectangle drawn under a cull mode that keeps it counts as covering.
bool ShadowCullCover();
// GXV_FIRSTCLEAR (gpucut23, default 0): the same mark for the back end's depth plan (vgx_backend_gl.cpp PlanFrame).
bool FirstClearMark();
void ShadowFillCounters(uint64_t out[5]);
// GXV_SHADOW_RECT (shadowpx23; vgx_draw.cpp "shadow rectangle"): 0 off (default), 1 on, 2 worked out and logged only (nothing changes).
// With 1 or 2 the alpha bounds of GXV_SHADOWFILL_BOUNDS are worked out (ShadowFillOn); with 1 its fill cuts are made too.
int ShadowRectMode();
// GXV_SHADOW_RECT_FE (shadowfe24; vgx_draw.cpp "shadow fill bounds"): 0 (default) the alpha bounds as they were, 1 the same bounds for less front-end work
// (a memo of the last bound, repeated vertices skipped, shared products, no deferred-key copy for a draw that cannot write alpha), 2 both ways compared.
int ShadowRectFeMode();
// (tests, logs) GXV_SHADOW_RECT_FE totals since the start: memo hits, misses, vertices skipped as repeats, points worked out, key copies not made, draws
// compared in mode 2, and of those the ones that differed (must be 0).
void ShadowRectFeCounters(uint64_t out[7]);
// (tests) GXV_SHADOW_RECT totals since the start: captures marked with their rectangle, captures left whole (and why: the alpha
// may be other than 0 everywhere / an alpha writer of unknown pixels / the capture's shape), and the sum of the marked rectangles'
// areas in thousandths of the capture.
void ShadowRectCounters(uint64_t out[6]);
// GXV_SHADOWFETCH (gpu23b; vgx_draw.cpp ShadowFetchPass, vgx_backend_gl.cpp "shadow fetch"): the mode (read once: 0 off, 1 on, 2
// calibrate and check only), and (tests) totals since the start: pictures, fill pairs found, fetched (copy left out), calibration
// requests, stale reads. The back end's word on fill keys and on whether it can fetch at all (it calls these; weak there).
int ShadowFetchMode();
void ShadowFetchCounters(uint64_t out[5]);
void ShadowFetchPublish(uint64_t key, int state); // state 1: exact (fetch), 2: refused for good
void ShadowFetchUsable(int on);
// GXV_SHADOW_STENCIL (pass24; vgx_draw.cpp ShadowStencilPass, vgx_backend_gl.cpp "shadow stencil"): 0 off (default), 1 on, 2 the pictures are
// analysed and counted, nothing changes; 3 (stencil43) as 2 and a "stencil classes:" census line every 600 pictures (draws by class, windows by what they hold, how often
// the path would now be taken and what made it possible). The back end says whether it has a stencil surface on a single-sample EFB. Counters (tests, the log):
// [0] pictures, [1] shadow windows (a clear quad, volumes, a capture), [2] stale reads, [3] draws given a stencil operation,
// [4 + r] windows by reason r (ShadowStencilReasonName: 0 taken, 2.. fell back).
int ShadowStencilMode();
// GXV_SILH_GATE (shadow25), stage-settable: 0 off (default), 1 on, 2 census (counts what it would do, changes nothing). See vgx_draw.cpp "silhouette gate".
int SilhGateMode();
// GXV_SILH_OCC (across26, stage-settable `silh_occ`): 0 off (default), 1 the gate also needs the occlusion query's all-clear (hold 12 pictures, a hit must have been seen once),
// 2 the same with hold 5 and no proof needed, 3 census (queries run and are counted, no decision changes). Acts only with GXV_SILH_GATE=1 (census: GXV_SILH_GATE=2).
// See vgx_silhocc.h; the history is shared with the back end (vgx_silhocc_gl.h).
int SilhOccMode();
silhocc::History& SilhOccHistory();
silhocc::Params SilhOccParams();
void SilhHintMarker(uint32_t v); // PORT_GX_MARK_SILH_HINT (BP 0xFF kind 32, the hint in bits 8-15)
int TailFoldMode(); // GXV_TAILFOLD (0 off, 1 / 2 count, 3 apply; read once): vgx_draw.cpp
int BloomBracketMode(); // GXV_BLOOM_BRACKET (0 off, 1 on, 2 count only; read once)
int CornerPassMode(); // GXV_CORNERPASS (imp45; 0 off, 1 on, 2 count only; read once): vgx_draw.cpp marks the copy, vgx_backend_gl.cpp PlanCornerPass makes it
void CornerPassAutoOff(int off); // (cp50) 1: the stage's marked copies were all left alone: the front end stops marking and works no alpha bounds for it; 0 at a stage load
void CornerPassFeFill(uint64_t out[2]); // (fix47) copies marked on a stencil window's fill certificate, certified fills met
void CornerPassFeCounters(uint64_t out[6]); // (tests, the back end's line) candidates seen, marked, then not marked: rectangle new or changed, alpha not known to be 0, no writer, EFB target changed
void BloomBracketMarker(uint32_t sub); // PORT_GX_MARK_BLOOM_BRACKET (BP 0xFF kind 40, the sub-kind in bits 8-15): vgx_draw.cpp
void BloomBracketCounters(uint64_t out[15]); // (tests) pictures with a bracket, brackets, dead, dropped, then the refusals by vgx_bloombracket.h Reason (4 + reason)
void SceneDepthMarker(); // hrw41: PORT_GX_MARK_SCENE_DEPTH (BP 0xFF kind 7): the next draw or EFB copy carries depthMark
void SilhGateCounters(uint64_t out[16]);
void ShadowStencilUsable(int on);
void ShadowStencilCounters(uint64_t out[24]);
const char* ShadowStencilReasonName(int r);
const char* ShadowStencilCauseName(int c); // (ss24) the name of a culprit's cause (ShadowStencilCulprits word 7)
void ShadowStencilExtra(uint64_t out[24]); // (ss24) the verify counters of GXV_SS_TILEBOUND / GXV_SS_FOGALPHA and the fill's refusals: see vgx_draw.cpp
// (dread25) the draws behind "unclassified alpha writer" (8 words each, see vgx_draw.cpp), cleared by the call; returns how many (at most cap).
int ShadowStencilCulprits(uint32_t* out, int cap);
// Ends the frame on the player's side: closes the command buffer and hands it
// to the back end. `direct`: the caller is the render thread (the back end is
// run in place); otherwise the buffer goes through SubmitToBackend.
// `onDecoded` is called between the two, when nothing of the frame is left to
// be read from guest memory.
void FrontPresent(bool direct, void (*onDecoded)());
// When set, a finished frame's command buffer is handed to this function instead of being
// executed (or submitted) by FrontPresent. It may swap the buffer for an empty one and return
// before the frame has been drawn (vgx_play.cpp: back end on its own thread).
void SetFrameSink(void (*sink)(CmdBuf& buf));
void QueueSetTarget();
CmdBuf& Pending();
uint64_t CurrentFrame();
// GXV_TRACE=<frame>. (Asked for every draw and every list call: the answer is kept until the frame changes,
// g_traceNow -1 = not known yet.)
extern int8_t g_traceNow;
bool TraceFrameSlow();
inline bool TraceFrame() { return __builtin_expect(g_traceNow >= 0, 1) ? g_traceNow != 0 : TraceFrameSlow(); }

// ---- textures (vgx_texcache.cpp)
// Resolves what is bound on a texmap to a back-end texture id, decoding and
// queueing an upload when the guest data is new or changed.
bool ResolveTexture(int texMap, TexBind& out, uint16_t& width, uint16_t& height);
// pix23 (GXV_PIX_ZERO / GXV_PIX_ATPASS on): the smallest and largest byte of each sampled channel (R, G, B, A as the sampler returns
// them) of the texture the back end holds under this id, worked out when it was uploaded (every level sent). False: not known (an EFB
// copy, a replacement image, a compressed format whose colours the GPU decodes, the switches off).
bool TexChannelRange(uint32_t texId, uint8_t lo[4], uint8_t hi[4]);
bool PixTexRangesOn(); // whether uploads record them (either switch on)
void PixCounters(uint64_t out[8]); // (tests) pix23's counts since the start (vgx_draw.cpp)
extern uint32_t g_tlutLoadGen; // bumped whenever palette memory changes (BpTlutLoad): the texture binding memo's palette test
// Shader warm-up list (vgx_draw.cpp): every shader key and program pair that is ever needed is
// appended to this file, and the next run generates and links all of them in its first frame
// instead of in the middle of play. Call before the first frame (or GXV_SHADERLIST=<path>).
void SetShaderListFile(const char* path);
// The warm-up list's replay goes into the first command buffer's `warm` list (CmdBuf::warm) so that the back end does it in slices
// over the frames that follow instead of all in that frame (the game waits for the first frame: the Vita's 7 s of compiling and linking
// held up everything else the game starts with, loading included).
void SetShaderWarmupDeferred(bool deferred);
void PassCheckReport(FILE* f);           // result of GXV_PASSCHECK=1, for the statistics report
void TexCacheEndFrame();                 // frees least recently used textures when over the budget
void SetTextureBudget(uint64_t bytes);   // RGBA8 bytes of guest textures to keep; 0 = unlimited (also GXV_TEXBUDGET=<MiB>)
uint32_t AcquireCopyTexture(const void* dest, uint16_t w, uint16_t h, uint32_t fmt);
uint32_t AllocTextureId(); // a back-end texture id that belongs to no guest texture (copy atlases)
void DestroyCopyTexture(const void* dest);
// polish23 (GXV_COPY_DROP): the EFB-copy textures with a destination in [lo, hi) (a destroyed scene heap): counted, freed when `drop`.
// Front-end thread only. And for the census line: the copy textures alive, the texture ids handed out so far (never reused).
unsigned CopyTexturesInRange(const void* lo, const void* hi, bool drop);
unsigned CopyTextureCount();
uint32_t TextureIdsMade();
// grow24 (GXV_TEX_SCENEDROP, GXV_TEX_IDREUSE; vgx_texcache.cpp): a destroyed scene's guest textures. Front-end thread.
struct TexSceneDrop {
    int mode = 0;
    unsigned cacheBefore = 0, cacheAfter = 0; // cache entries before / after
    unsigned inRange = 0;       // entries whose image lies in the destroyed heaps
    unsigned idleOutside = 0;   // entries outside them that no frame bound since the previous scene was destroyed
    unsigned freed = 0;         // textures freed (CMD_TEX_FREE); and entries erased that had no texture yet
    unsigned freedNoTexture = 0;
    uint64_t freedBytes = 0, liveBytes = 0; // texel bytes freed; held after
    unsigned idsMade = 0, idsFree = 0, idsReused = 0; // ids ever made (high-water), free or in quarantine, handed out again so far
    // texage24 (mode 3 and the census): the age rule and the cap, the scene's pictures in play, and the refetch census (totals so far)
    unsigned aged = 0, freedCap = 0, playPics = 0;
    int age = 0;
    bool shortScene = false;    // fewer pictures in play than GXV_TEX_SCENEDROP_MINFRAMES: no idle count was raised
    unsigned refAddr = 0, refAddrLive = 0, refContent = 0, refContentLive = 0, adopted = 0; // freed, then decoded again: same address and bytes / same bytes elsewhere (live: in play)
    unsigned refAddrScene = 0, refContentScene = 0;  // of them, since the previous scene was destroyed
};
int TexSceneDropMode();  // GXV_TEX_SCENEDROP, read once: 0 off, 1 the destroyed heaps' textures, 2 and those idle through the scene, 3 by age and cap
TexSceneDrop TextureCacheSceneGone(const void* const lo[], const void* const hi[], int n, int mode);
unsigned TextureIdsFree();   // ids released and waiting for reuse (quarantine included)
unsigned TextureIdsReused(); // ids handed out again so far
// hdcensus (GXV_TEXMAG_CENSUS, vgx_texmag.inc): texture magnification census; all front-end thread. TexMagEnabled reads the switch (once).
bool TexMagEnabled();
void TexMagNoteTexture(uint32_t texId, uint64_t hdKey, uint32_t w, uint32_t h, uint32_t fmt); // at a guest texture's upload (hdKey as the HD pack's key)
void TexMagSceneEnd(); // a scene was destroyed: write the stage's file
size_t TextureCacheEntries(); // (heap23b) guest textures the cache has entries for (front-end thread)
void InvalidateAllTextures();
void TexCacheReport(FILE* f);
void SetGuestWriteGenerationHook(uint64_t (*fn)(const void*, size_t));
// Optional: called on the player's thread for every texture upload (texture, GX format, frame).
void SetTextureDumpHook(void (*fn)(const TexUpload& up, uint32_t gxFormat, uint64_t frame));
uint64_t LastUploadHash(); // (inside the dump hook: the content hash of the texture being uploaded)
// A missing predicate keeps candidate compact textures in RGBA so legacy hooks still run.
void SetTexturePatchHook(void (*fn)(uint64_t hash, uint32_t width, uint32_t height, uint8_t* rgba),
                         bool (*needsRgba)(uint64_t hash, uint32_t width, uint32_t height) = nullptr);
// settingsnative: the patch hook's result changed (the button mapping was swapped): every texture the hook patches is decoded and uploaded again at its
// next use. Any thread; a relaxed counter the texture cache compares per entry.
void TexturePatchChanged();
// Optional replacement of a guest texture when it is uploaded (vgx_hdtex.cpp installs one): `fn`
// may fill `up` (width, height, format, levels, authored) with another image of the texture and
// return true. Asked only on upload, never for EFB copies nor for textures the patch hook wants.
// The guest's dimensions stay what the draw state and the TEV see; the replacement carries its
// own mip chain, and its binds are given the mip filter `mipFilter` (FILT_LIN_MIP_*).
struct TexReplaceQuery {
    const uint8_t* data;  // level 0 in guest memory
    uint32_t fmt;         // GX texture format
    uint16_t width, height;
    const uint8_t* tlut;  // palette of a CI texture (null otherwise)
    uint32_t tlutBytes;
    bool mipClamp;        // backend::Caps::mipClamp: no compressed level below one 4x4 block
    uint64_t liveBytes;   // bytes of replacement textures the back end holds now
    bool* inPack = nullptr; // (not null) set to true when the texture is one of the pack's, replaced or not (GXV_CHARAA)
    bool* player = nullptr; // (not null) set to true when it is one of Mario's or Luigi's (GXV_CHARAA_AUTO)
};
void SetTextureReplaceHook(bool (*fn)(const TexReplaceQuery& q, TexUpload& up), uint8_t mipFilter);
uint64_t ReplacedTextureBytes(); // bytes of replacement textures alive in the back end
// hdtex24: what the cache did with replacement textures (the basic log's "hdtex: scene" line): uploads and their bytes (of them in play: a stage is
// being played and no fade or pause hides it), freed again (scene drop, budget), alive now (a scan of the cache: front-end thread, once a scene).
struct HdTexStats {
    uint64_t uploads = 0, bytes = 0, uploadsPlay = 0, bytesPlay = 0, freed = 0, freedBytes = 0, alive = 0, aliveBytes = 0;
};
void HdTextureStats(HdTexStats& s);
// A texture of the HD pack's list (characters and key items: tools/hdtex/selection.txt), whatever was uploaded for it
// (GXV_CHARAA: their draws are the ones smoothed). False for EFB copies and unknown ids.
bool IsCharaTexture(uint32_t texId);
// The same, restricted to the player's own: the archives of Mario and Luigi in the list (GXV_CHARAA_AUTO).
bool IsPlayerTexture(uint32_t texId);
// mario24 (GXV_PLAYER_*, the "player-look:" line): what the cache knows of the player's textures (the pack's list from the archives of Mario and
// Luigi), as last uploaded: the size the back end holds (a replacement: the HD size) and the game's own, the mip levels it holds, whether it is an
// HD replacement, the GX format, and the minification filter its binds get (0 nearest, 1 linear, 2..5 the mipmapped ones: FILT_*). Newest last; returns
// the number written (at most `max`). Any thread (a short lock).
struct PlayerTexFact {
    uint32_t texId;
    uint16_t w, h, origW, origH;
    uint8_t levels, gxFmt, origMin, finalMin;
    bool hd;
};
constexpr int kPlayerTexFactsMax = 48;
int PlayerTexFacts(PlayerTexFact* out, int max);
// GXV_PLAYER_MIPVIS=1 (diagnostic, default 0; read once): every uploaded texture of the player's is sent as a flat colour per mip level (level 0 red,
// 1 green, 2 blue, 3 yellow, 4 magenta, 5 cyan, 6+ white), so a paused picture shows which level the GPU reads where.
bool PlayerMipVisOn();

// ---- edge smoothing of the characters and key items (GXV_CHARAA, vgx_draw.cpp "character edge smoothing")
int CharaAAMode(); // GXV_CHARAA, read once: 0 off, 1 on, 2 on with the smoothed areas tinted (diagnostic)
// GXV_EDGE_AA (vgx_draw.cpp, "whole-picture edge smoothing"), read once: 0 off, 1 on, 2 tinted (diagnostic), 3 left half only
int EdgeAAMode();
// GXV_HUD_NATIVE (vgx_draw.cpp, read once): 0 off, 1 the 2D layer is drawn at the display's size, 2 plan and count only. With it on the front end flags the 2D draws.
int HudNativeMode();
// GXV_EDGE_AA totals since the start: display copies, smoothed whole, HUD rectangles handed over, given up (HUD area), HUD draws measured
void EdgeCounters(uint64_t out[5]);
// The model-space bounds of a cached list's vertices by position-matrix row (vgx_dlcache.cpp computes them once per list;
// the first is all vertices whatever their row, for draws with one matrix).
struct RowBox {
    float mn[3], mx[3];
    uint32_t row;
    float c[3] = {}, r = 0; // the bounding sphere of mn..mx (what the projection uses: RowBoxesOf keeps it up to date)
    uint32_t drawn = 0;     // ([0] only, GXV_STATICCULL) the list has been drawn once since it was built (its first use is never culled)
};
// Before EmitStaticRun / EmitDraw of a cached list: its bounds (n boxes; the next emitter call consumes them).
void SetPendingBounds(const RowBox* boxes, uint32_t n);
// GXV_CLUSTERCULL (vertex25, vgx_draw.cpp "clusters of a static run"): 0 off, 1 on, 2 dry (counts what it would leave out, draws all), 3 on + checks.
// The cluster table of a promoted list's rigid triangle run: `n` boxes (RowBox mn / mx; `row` = the cluster's first index, counted from the run's
// first index, a multiple of 3; the cluster ends where the next begins, the last at the run's end). The next EmitStaticRun consumes them.
int ClusterMode();
void SetPendingClusters(const RowBox* cl, uint32_t n);
// Build time (vgx_dlcache.cpp Promote): the limits (triangles per cluster, smallest run in indices) and the counters of the checks.
// ta27 (vgx_ta27.h; vgx_draw.cpp "ta27 census"): GXV_TA27 = 0 off (default), 1 census (the "ta27:" line every 10 s: what a normal-cone cull and a spatial
// re-clustering of the static triangle runs would leave out; nothing is drawn differently), 2 census + build checks. Read once.
// A promoted list's runs get, when it is on, the clusters of Entry::t27 (vgx_dlcache.cpp BuildTa27); the next EmitStaticRun consumes the pointer.
int Ta27Mode();
uint32_t Ta27Tris();
uint32_t Ta27MinRun();
void SetPendingTa27(const ta27::RunSet* s);
void Ta27BuildNote(uint32_t runs, uint32_t clusters, uint32_t bad);
uint32_t ClusterTris();
uint32_t ClusterMinRun();
void ClusterBuildNote(uint32_t runs, uint32_t clusters, uint32_t boxBad);
void RowBoxesOf(const uint8_t* verts, uint32_t count, uint32_t stride, std::vector<RowBox>& out); // (appends; out[0] = all)
// GXV_STATICCULL (vgx_draw.cpp, "static runs outside the view"): a static run whose list's bounds are outside the clip volume is
// not drawn. Enabled: the display-list cache works the bounds out for every list it builds. Counters since the start: static
// runs tested, culled, and the vertices of the culled ones.
bool StaticCullEnabled();
void StaticCullCounters(uint64_t out[3]);
bool StaticCullPalEnabled();         // GXV_STATICCULL_PAL (ta23): the same for runs with per-vertex matrices, by the per-row boxes
uint32_t DynCullMin();                // GXV_DYNCULL (ta23): dynamic draws of at least this many vertices are tested (0: off)
void CullCounters23(uint64_t out[8]); // (tests) totals of the two above (vgx_draw.cpp)
// GXV_OCCLCULL (geopass23; vgx_geopass.inc "hidden behind a closed occluder"): a static run of a cached list that is a closed, consistently
// wound surface with an inscribed ball (worked out once, when the list is promoted: OcclAnalyse) is an occluder once drawn with depth
// writes; later depth-tested draws of the same picture and view whose every point lies in the ball's shadow (the cone from the eye
// behind the ball, deeper than the ball by a margin) are not drawn. Read once: 0 off, 1 on, 2 count only (nothing left out).
struct OccShape {
    float c[3] = {}, r = 0.0f, R = 0.0f; // model space: the inscribed ball (centre, radius) and the surface's bounding radius about c
    int8_t orient = 0;                    // +1: right-hand normals point out of the surface, -1: in; 0: not an occluder
    uint8_t analysed = 0;                 // (census) 1: OcclAnalyse looked at it; 2: left out by the analysis budget
    uint32_t tris = 0;                    // (census) its triangles
    uint32_t bEdges = 0;                  // (census) directed edges whose reverse the run does not have (0: closed)
    uint64_t bSum = 0, bRev = 0;          // (census, GXV_GEOPASS) sums of hashes of those edges and of their reverses: a group of runs drawn
                                          // with one matrix is closed when its bSum and bRev sums agree
};
int OcclCullMode();
// Analyses one run (triangle-list indices idx[0..idxCount) into the vertices at vtx + addrBase + index * stride, positions first):
// fills s (s.orient stays 0 for anything that is not a closed, consistently wound surface with a ball inside).
void OcclAnalyse(OccShape& s, const uint8_t* vtx, size_t vtxBytes, uint32_t stride, uint32_t addrBase, const uint16_t* idx, uint32_t idxCount);
// Before EmitStaticRun (consumed by it): the run's analysis (nullptr: none).
void SetPendingOccluder(const OccShape* s);
void OcclCounters(uint64_t out[8]); // (tests) since the start: occluders registered, static runs left out, their indices, dynamic draws
                                    // left out, their indices, runs analysed, closed runs found, epoch breaks
// GXV_GEOPASS (geopass23, diagnostic, read once; 0 off): the "geopass:" basic-log line every 10 s: the picture's EFB scenes (draws, indices,
// the copy that ended each), the GX draws and indices of each game draw phase (PORT_GX_MARK_PHASE markers, written by the game only
// with this switch on), geometry drawn more than once a picture, and the occluder census.
int GeoPassMode();
// bglayer25 (vgx_bglayer.inc): GXV_BGLAYER (0 off, 1 layer cache, 2 census only), read once; PORT_GX_MARK_BGCAT (BP 0xFF kind 28, the draw category in bits 8-15).
int BgLayerMode();
void BgMarker(uint32_t kind, uint32_t payload); // PORT_GX_MARK_BGCAT (28: the category in bits 8-15 of the word), BG_BEGIN / BG_END / BG_REUSE (29..31: the view slot in bits 8-23)
// the view the game hands over for a layer picture (written before its marker; the front end reads it at the marker): the 3x4 world-to-eye matrix and the projection's
// x / y scales (NDC per unit of slope); a test calls it directly. GXV_BGLAYER_* counters: pictures rendered, reused, misses, grid draws.
void BgSetView(uint32_t slot, const float view12[12], float p00, float p11);
void BgLayerCounters(uint64_t out[8]);
void GeoPhaseMarker(uint32_t phase); // PORT_GX_MARK_PHASE (BP 0xFF kind 11, the phase in bits 8-15)
void CharaMarker(uint32_t kind); // PORT_GX_MARK_* arriving in the stream (BP 0xFF, see WriteBP)
// prewarm24 (vgx_draw.cpp): GXV_PREWARM, read once (0 off, 1 the stages' effects behind the closed screen with a budget, 2 the pack tour: no
// budget, every resource); PORT_GX_MARK_WARM_BEGIN / _END (BP 0xFF kinds 14 / 15) arriving in the stream; whether a scope is open; the
// counters (PrewarmCounters: scopes, draws, variants derived, queued, unfolded twins, other-path draws taken, unmatched markers).
long PrewarmMode();
// layoutwarm24 (vgx_draw.cpp): GXV_LAYOUTWARM (0 / 1) and GXV_LAYOUTWARM_LOG (0 / 1), read once; whether the front end honours the warm-up scope
// markers (for the effects or the layouts); PORT_GX_MARK_LAYOUT_BEGIN / _END (BP 0xFF kinds 19 / 20, the census id in bits 8-23).
long LayoutWarmMode();
long RehearseMode(); // (dive27 GXV_REHEARSE, vgx_draw.cpp)
bool LayoutCensusOn();
bool WarmScopeHonoured();
void LayoutCensusMarker(uint32_t kind, uint32_t id);
void WarmScopeMarker(uint32_t kind);
bool WarmScopeActive();
// precomp26 (vgx_draw.cpp): GXV_STAGE_ENUM, read once (0 off, 1 census, 2 on); PORT_GX_MARK_STAGE_ENUM_DRAW (BP 0xFF kind 33, the arg in bits 8-23) arriving in the stream; the
// counters (StageEnumCounters); the inbox the back end drains (vgx_stage_enum.h).
long StageEnumMode();
void StageEnumDrawMarker(uint32_t arg);
void StageEnumCounters(uint64_t out[7]);
senum::Inbox& StageEnumInbox();
void PrewarmCounters(uint64_t out[7]);
void TransientMarker(uint32_t stage); // PORT_GX_MARK_TRANSIENT_COPY with its 16-bit stage tag (0: none), GXV_INDCAP_SKIP
void CapReadCounters(uint64_t out[7]); // (tests) GXV_CAPREAD: pictures, whole-EFB copies, readers before / after them, pictures logged, reader lines, left out (HIDE)
void PrevCapCounters(uint64_t out[16]); // (tests) GXV_PREVCAP: pictures, marked, left out, reader draws redirected, earlier-picture readers redirected, keeps, refusals (no previous, cut, alpha, atlas, no reader, shape, direct, skipped)
void IndCapCounters(uint64_t out[8]); // (tests) GXV_INDCAP_SKIP totals: pictures, marked, left out, kept, left out by the half rate, stale reads,
                                       // kept for readers of the previous picture's capture (PREDICT / HOLD), such reads
void IndCapRunCounters(uint64_t out[4]); // (tests) GXV_INDCAP_REFRESH made, kept in the warm-up, kept for a learned stage, stages learned
uint64_t TevInPrewrapKeys(); // (tests) GXV_TEVIN_PREWRAP: fragment keys built with a wrapped-copy row
int ClearQMode(); // GXV_CLEARQ (pixcuts25), read once, stage-settable: 0 off, 1 the game's EFB clear quad as a constant glClear, 2 count only
int ClearQFrontCensus(char* buf, unsigned cap); // ("clearq:" line)
void ClearQFrontCounters(uint64_t out[8]); // (tests, "clearq:" line) quads seen, marked, refused by reason: key, state, colour, depth, alpha, extent
int IndCapSkipMode(); // GXV_INDCAP_SKIP / _HALFRATE, read once: 0 off, 1 leave out unread "Indirect" captures, 2 count only (verify), 3 half rate alone
constexpr uint32_t kPresentAllowedMarker = 0x10;
constexpr uint32_t kPresentSuppressedMarker = 0x11;
void PresentationMarker(uint32_t kind);
// GXV_PRESENT_SYNC (vgx_galaxy.cpp port_vgx_present_turn): the vblank of the turn whose draws follow, in bits 8-23 of the value.
constexpr uint32_t kPresentStampMarker = 0x12;
void PresentationStamp(uint32_t vblank16);
// GXV_HRW (hrw26, vgx_hrw.h): 0 off, 1 on, 2 verify-still, 3 census. Read once. The game writes PORT_GX_MARK_HRW (kHrwMarker) with the pair's number in bits 8-23 just before the
// draws of a picture it opens a pair with; the front end puts the number on the picture's CmdBuf (hrwId) and the render thread keeps that buffer for the replay.
constexpr uint32_t kHrwMarker = hrwpair::kMaybeFresh;
int HrwMode();
void HrwMarker(uint32_t token);
// hrw27: 0 off, 1 on, 2 verify-still, 3 census, 4 verify-fresh. PORT_GX_MARK_OWNER (kOwnerMarker, the key in bits 8-23, 0 none) names the owner of the draws that follow (DrawRec::owner);
// PORT_GX_MARK_FRESH_END (kFreshEndMarker, the pair's number in bits 8-23) ends the game's partial draw: the buffer made since the picture before goes to the render thread as a replay job
// (FrontFresh: FrontPresent's hand-over without a picture: no display copy, no frame number, no passes).
constexpr uint32_t kOwnerMarker = 0x23;
constexpr uint32_t kFreshEndMarker = 0x24;
void OwnerMarker(uint32_t key16);
void FreshEndMarker(uint32_t id16);
void FrontFresh(uint32_t id16);
// Totals since the start: display copies, of them with rectangles, character draws, of them without usable bounds, HUD draws
// measured, rectangles handed to the back end, their EFB pixels, frames over GXV_CHARAA_AREA, rectangles dropped by clearing
// copies, pieces lost to HUD cuts, rectangles too small to smooth (GXV_CHARAA_MIN).
void CharaCounters(uint64_t out[11]);

// ---- statistics (vgx_stats.cpp)
namespace stats {
void TexFormat(uint32_t fmt, uint16_t w, uint16_t h, bool mip);
void CopyFormat(uint32_t fmt, bool disp);
// The per-draw bookkeeping below (feature usage, distinct textures / programs per frame) is
// only done while Detailed() is on: the host layer turns it on when a statistics report or the
// per-frame CSV is asked for. It costs about 6% of the player's time otherwise.
void SetDetailed(bool on);
extern bool g_detailed;
inline bool Detailed() { return diag::kEnabled && g_detailed; }
void Draw(const FsKey& fs, const VsKey& vs, uint32_t layoutMask, uint32_t verts);
void VertexFormat(int attr, int type, const VtxAttrFmt& f);
void Note(const char* what); // one-off feature flag, e.g. "indirect"
void ShaderCreated(bool fragment, uint64_t frame);
void FrameEnd(uint64_t frame);
void CloseFrameLog();
int ExportFrameLog(); // Paused consumer; reads only the immutable RAM CSV prefix.
void Dump(const char* path);
// Per-frame counters (one CSV line per presented frame, GXV_FRAMESTATS=<file>).
struct Frame {
    uint32_t prims = 0;        // GX primitives submitted (GXBegin / draw opcodes)
    uint32_t primsDL = 0;      // of those, from display lists / raw streams
    uint32_t vertsDL = 0;      // vertices of those
    uint32_t verts = 0;        // vertices decoded
    uint32_t tris = 0;         // triangles produced
    uint32_t drawCalls = 0;    // draw records after merging (= GL draw calls)
    uint32_t streams = 0;      // command streams (display lists, XF packets) parsed
    uint32_t streamBytes = 0;
    uint32_t texUploads = 0, texUploadBytes = 0; // texture bytes handed to the back end
    uint32_t texHashes = 0, texHashBytes = 0;   // re-validations of cached textures by content hash
    uint32_t texUntracked = 0;                  // of those: data outside tracked guest RAM (hashed on every bind)
    uint32_t texEvicted = 0, texEvictBytes = 0; // freed to stay inside the texture budget
    uint32_t copies = 0;       // EFB-to-texture copies
    uint32_t copyPixels = 0;   // destination texels of those
    uint32_t deadCopies = 0;   // of the copies: never sampled before being overwritten (only their clear is executed)
    uint32_t passes = 0;       // of the copies: end of an isolated offscreen pass (source is the pass target)
    uint32_t passDraws = 0;    // draws inside isolated passes
    uint32_t passesDirect = 0; // of the passes: can be drawn straight into the copy's destination texture
    uint32_t fills = 0;        // of the copies: source rectangle known to be a constant colour
    uint32_t fillsRedundant = 0; // of the fills: the texture already holds exactly that (nothing is executed but the clear)
    uint32_t efbReads = 0;     // of the copies: the remaining ones, which really read the EFB
    uint32_t clearsRedundant = 0; // clears (of copies) not executed: the rectangle already holds exactly those values
    uint32_t atlasPasses = 0;  // of the direct passes: drawn into a cell of a copy atlas
    uint32_t atlasRemaps = 0;  // draws whose texture coordinates were rewritten to sample an atlas cell
    uint32_t atlasMerged = 0;  // draw records merged into their predecessor once they sampled the same atlas
    uint32_t atlasEvicted = 0; // pictures taken back out of an atlas (sampled in a way that cannot be redirected)
    uint32_t atlasSorted = 0;  // runs of consecutive atlas passes put into page order
    // What a back end with one render target per framebuffer object does with the frame:
    uint32_t efbScenes = 0;    // stretches of drawing / clearing in the EFB between uses of other targets
    uint32_t offScenes = 0;    // uses of other targets: offscreen passes, copy quads, fills
    uint32_t shadersVs = 0, shadersFs = 0; // generated this frame
    uint32_t vtxBytes = 0, idxBytes = 0, uniBytes = 0;
    uint32_t softVerts = 0;    // vertices transformed on the CPU
    uint32_t dlHit = 0, dlBuild = 0, dlRehash = 0, dlBypass = 0; // display-list cache (vgx_dlcache.cpp)
    uint32_t staticDraws = 0, staticVerts = 0; // draws from static buffers
    uint32_t bufUploads = 0, bufUploadBytes = 0;
    uint64_t hash = 0;         // GXV_HASH=1: hash of the frame's command buffer(s)
};
extern Frame g_curFrame;
inline Frame& Cur() { return g_curFrame; }
void TexBound(uint32_t texId, uint32_t bytes); // distinct textures referenced by this frame's draws
void ProgramUsed(uint16_t vsId, uint16_t fsId);
void TexLive(int32_t deltaCount, int64_t deltaBytes); // textures alive in the cache
}

} // namespace vgx
