// Front-end GX state: the hardware register file (BP / XF / CP) plus the
// bookkeeping the HLE-level API adds on top (texture objects, vertex arrays).
// API calls are encoded into register writes exactly like the SDK does, and
// command streams (display lists) are parsed into the same writes, so there
// is a single source of truth for both paths.
#pragma once
#include <cstring>

#include "vgx_cmd.h"
#include "vgx_shadergen.h"

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
};
void SetGuestHooks(const GuestHooks& h);
const GuestHooks& Hooks();
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
bool StateListFastHit(const uint8_t* list, uint32_t nbytes);  // (CallList's shortcut for the common case)
void StateListCounters(uint64_t out[5]); // hits, builds, refused, verify checks, verify mismatches
// The fecache switches' counters, for the host's status line (totals since start):
void RegShadowCounters(uint64_t out[2]);      // GXV_REGSHADOW=2: writes checked, mismatching
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
void DlCacheReport(FILE* f);
uint64_t DlCacheLiveBytes();   // decoded vertices the front end keeps for cached lists (a count the player thread updates; readable elsewhere for logs)
uint64_t DlCacheStaticBytes(); // bytes of the static buffers the back end holds for promoted lists
void DrawStream(uint8_t prim, int fmt, const uint8_t* data, uint32_t count, uint32_t availBytes);
// Byte ranges of the vertex arrays (attributes POS..TEX7) a decode read.
struct ArraySpans {
    uint32_t mask = 0; // bit per attribute number
    uint32_t lo[12] = {}, hi[12] = {};
};
const Plan& CurrentPlan(int fmt); // the player's plan for a vertex format under the current descriptor
ArraySpans MeasureArraySpans(const Plan& p, const uint8_t* src, uint32_t count);
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
bool EmitDraw(uint8_t gxPrim, uint32_t layoutMask, uint32_t count, uint32_t foldedFrom = 0xFFFFFFFFu);
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
uint32_t LastDecodedPosMtxMax(); // of the last DecodeVertices call
// Number of indices a GX primitive of `count` vertices turns into; writes them (relative to `base`) when out != null.
uint32_t PrimIndices(uint8_t gxPrim, uint32_t count, uint32_t base, uint16_t* out);
uint32_t SoftXformLimit(); // primitives up to this many vertices may be transformed on the CPU (and merged)
// GXV_HASH=2 needs the bytes of a static buffer to hash what a draw refers to (vgx_dlcache.cpp).
bool StaticBufferData(uint32_t bufId, const uint8_t*& vtx, const uint16_t*& idx);
void QueueCopyTex(const void* dest, bool clear);
void QueueCopyDisp(bool clear);
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
void InvalidateAllTextures();
void TexCacheReport(FILE* f);
void SetGuestWriteGenerationHook(uint64_t (*fn)(const void*, size_t));
// Optional: called on the player's thread for every texture upload (texture, GX format, frame).
void SetTextureDumpHook(void (*fn)(const TexUpload& up, uint32_t gxFormat, uint64_t frame));
uint64_t LastUploadHash(); // (inside the dump hook: the content hash of the texture being uploaded)
// A missing predicate keeps candidate compact textures in RGBA so legacy hooks still run.
void SetTexturePatchHook(void (*fn)(uint64_t hash, uint32_t width, uint32_t height, uint8_t* rgba),
                         bool (*needsRgba)(uint64_t hash, uint32_t width, uint32_t height) = nullptr);
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
};
void SetTextureReplaceHook(bool (*fn)(const TexReplaceQuery& q, TexUpload& up), uint8_t mipFilter);
uint64_t ReplacedTextureBytes(); // bytes of replacement textures alive in the back end
// A texture of the HD pack's list (characters and key items: tools/hdtex/selection.txt), whatever was uploaded for it
// (GXV_CHARAA: their draws are the ones smoothed). False for EFB copies and unknown ids.
bool IsCharaTexture(uint32_t texId);

// ---- edge smoothing of the characters and key items (GXV_CHARAA, vgx_draw.cpp "character edge smoothing")
int CharaAAMode(); // GXV_CHARAA, read once: 0 off, 1 on, 2 on with the smoothed areas tinted (diagnostic)
// The model-space bounds of a cached list's vertices by position-matrix row (vgx_dlcache.cpp computes them once per list;
// the first is all vertices whatever their row, for draws with one matrix).
struct RowBox {
    float mn[3], mx[3];
    uint32_t row;
    float c[3] = {}, r = 0; // the bounding sphere of mn..mx (what the projection uses: RowBoxesOf keeps it up to date)
};
// Before EmitStaticRun / EmitDraw of a cached list: its bounds (n boxes; the next emitter call consumes them).
void SetPendingBounds(const RowBox* boxes, uint32_t n);
void RowBoxesOf(const uint8_t* verts, uint32_t count, uint32_t stride, std::vector<RowBox>& out); // (appends; out[0] = all)
void CharaMarker(uint32_t kind); // PORT_GX_MARK_* arriving in the stream (BP 0xFF, see WriteBP)
constexpr uint32_t kPresentAllowedMarker = 0x10;
constexpr uint32_t kPresentSuppressedMarker = 0x11;
void PresentationMarker(uint32_t kind);
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
