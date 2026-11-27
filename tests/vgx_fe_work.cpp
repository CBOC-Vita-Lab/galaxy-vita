// The player's per-picture work on a J3D-shaped scene (GXV_SLDELTA, GXV_RSMEMO: tools/test_vgx_fe_work.sh), through the real front end.
// The streams are shaped as the game's J3D packets make them (the command order and register sets of the material display lists of the
// Comet Observatory's models, tools/cook data, with values of its own): per material packet the material's list (texture images and
// modes, TEV order, masked texture-coordinate scales, TEV colours with the BG half written three times, konst colours, TEV stages,
// indirect stages, konst selections, masked indirect registers, fog, alpha compare, masked blend / pixel control / gen mode, texture
// matrices, texgens, channel colours and controls), its differed list (TEV colours, a texture matrix, a material colour) written again on
// every picture into the other of its two buffers (J3DDisplayListObj::swapBuffer), mostly with the same bytes, a few animated; per shape
// the vertex-format list (J3DShape::loadPreDrawSetting, only when it is another shape's), the position / normal matrix loads, the array
// bases, the geometry list. Textures are in guest RAM. Objects come and go and swap places.
// Prints a hash of every command buffer: runs with the switches on and off (and GXV_SPLIT=0 / 1) must print the same lines.
//   vgx_fe_work [frames] [seed]
//   FEW_PROFILE=1: no hashing or printing (callgrind: the player's instructions)
//   FEW_FX=<n>: n particle-like quads per picture (see g_fxCount)
//   FEW_JPA=<n>: n particles per picture as JParticle draws them (see g_jpaCount)
//   FEW_FXMERGE=1: those particles as the game's GX library sends them with GXV_FXMERGE=1 (one port-private command per emitter loop)
//   FEW_JPAVAR=1: more kinds of emitters among them (see g_jpaVar)
//   FEW_RETAINED=1: every shape's geometry call comes with its handle in front (opcode 0x59, a u32: what J3DShapeDraw::draw sends with
//                   GXV_RETAINED=1, decomp/src/RVL_SDK/gx/GXDisplayList.c port_gx_call_retained); FEW_RETAINED=0: the same stream
//                   pieces without the handles (compare the two). FEW_RETAINED=2: handles as 1, but some wrong: the handle of another
//                   shape, 0, or a handle with no call after it (from a sequence of their own)
//   FEW_J3N=1 (j3n1, GXV_J3DNATIVE): every material-list call and every differed-list call comes in a bracket (opcode 0x5A, id, versionA, versionB, ext,
//                   flags, the call, 0x5B: what decomp/src/RVL_SDK/gx/GXDisplayList.c port_gx_call_bracketed sends), the versions kept as the game's
//                   J3DDisplayListObj keeps them. FEW_DIFFKEEP=1: a differed list is stored (other buffer, version moved) only when its bytes changed
//                   (GXV_J3D_DIFFKEEP). FEW_LIGHT=<n>: the actor lights (XF 0x600..) written at top level, their values changing every n pictures.
//                   FEW_FOG=<n>: a top-level fog write that changes every n pictures. FEW_SCENE=<n>: every n pictures the scene is destroyed and
//                   made again (lists rewritten with other bytes in place, the store dropped; FEW_SCENE_REUSE=1: the ids and versions restart at
//                   the same numbers: hostile). FEW_J3N_MISS=1: the mutation test: a material list is rewritten without moving its version
//                   (a writer the game's bumps missed).
//   FEW_FUR=<layers>: furry models as Game/Util/FurCtrl.cpp draws them (GXV_DL_VARIANTS): FEW_FURN (default 6) of them a picture, each
//                    its own shape once with the model's arrays, then once per layer with the layer's own position array (made once, shared
//                    by every model), a TEV colour per layer and the model's matrices: one list, layers + 1 vertex states
//   FEW_PLANTS=<n>: n instances a picture of one plant model as DrawBufferShapeDrawer draws a flower group (the material's list once, the
//                    shape's vertex format once, then per instance only its matrices, the array bases and the shape's list: the differed
//                    lists are skipped, GXV_J3D_DIFFSKIP); for sizing instance batching (GXV_FE_CENSUS counts them as repeats)
//   FEW_INST=<n> (fedl25, GXV_RT_INST): n instances a picture of one model whose shape has ONE geometry list and ONE handle, each instance with arrays
//                    of its own (a copy of the model's position array; the odd ones a copy of the normals as well), as the game's per-instance vertex
//                    buffers are; every 5th picture one instance's positions are written again (new bytes, same list: the arrays change between
//                    pictures), every 11th the same bytes again (a write that changes nothing). From their own random sequence.
//   FEW_BIG=<n>: n large streamed draws per picture (GXV_FE_PARALLEL, vgx_fepar.h: decodes on the finishing thread): top-level draws with
//                direct attributes, with indexed ones (the general decode), with PNMTXIDX (which stay on the player), some small, some lines, and
//                some in a display list written again every picture (the cache bypasses it: ProcessStream), with draw-sync tokens between
//                them. From a random sequence of their own: the rest of the scene is the same with and without them.
//   FEW_SHADOW=<n>, FEW_SHADOWDYN=<n> (shadowfe24): n shadow volumes a picture as ShadowVolumeDrawer::draw sends them (decomp Game/LiveActor): per volume its
//                    position matrix, then twice the same shape - colour update off, destination alpha off, cull front / blend ONE ONE, then cull back /
//                    blend SUBTRACT, depth GEQUAL without update: FEW_SHADOW volumes as a retained J3D shape's static run (a model: ShadowVolumeModel,
//                    Cylinder, Oval ...), FEW_SHADOWDYN volumes as immediate geometry (ShadowVolumeBox: a 14-vertex strip and two 8-vertex fans).
//                    From their own random sequence: the rest of the scene is the same with and without them. These are the draws that write alpha
//                    other than 0, which GXV_SHADOWFILL_BOUNDS / GXV_SHADOW_RECT work the screen bounds of.
//   FEW_HASHAB=1: the hash also covers every record's alpha bounds (DrawRec::ab, abX.., coverCull)
//   FEW_CONSOLE=1 (twocore25): the console's stream shape around the J3D packets (fetwo: lines of c49, tw6 / tw7): most shape packets'
//                    differed lists are J3D's small ones (J3DShapePacket::calcDifferedBufferSize: a material colour, 13 bytes, or a texture
//                    matrix, padded to 32 / 64 bytes, starting with an XF write: the front end parses them inline, CallListBody's tiny branch);
//                    one shape in 6 is a small one whose geometry list is 64 bytes or less (one strip: drawn inline as a dynamic draw); one
//                    in 16 has a geometry list written again with other bytes every picture (the list cache bypasses it). The pieces are
//                    FEW_TINYDIFF=<n of 8 packets>, FEW_TINYGEO=<n of 24 shapes>, FEW_BYPASS=<n of 32 shapes>; FEW_CONSOLE=1 sets 6 / 4 / 2.
//                    From a random sequence of their own (unset: the scene is as before).
//   FEW_CENSUS=<n> (lighter25, GXV_STREAM_CENSUS): every n-th picture carries the game's census markers (platform/src/port/perf.cpp
//                    port_stream_census_*): BEGIN first, a source marker around each part (the J3D packets as opa-0F, plants opa-10, fur opa-11,
//                    shadow volumes part-shadow, fx quads exec-12, particles exec-30, big draws exec-31), a model marker (kind 22: model id, size
//                    code) around each material packet. Nothing else changes (no random value is drawn for them).
//   FEW_J3DFMT=1 (lighter25): the vertex-format list as J3DShape::makeVtxArrayCmd makes it (array bases and strides too: the same values the
//                    shapes write at top level) and called again at every material packet's first shape (J3DMatPacket::draw resets
//                    J3DShape's cache at each packet's end)
//   FEW_LIGHTER=<bits> (lighter25: what a lighter stream would save the front end; the pictures must stay the same): 1 a vertex-format call
//                    whose list is the one called last (nothing else wrote the vertex state since) is left out; 2 a shape's differed list
//                    that is the one the shape before it in the packet called is left out; 4 a top-level array base / stride / CP 0x30
//                    write of the value the register holds is left out; 8 the array-base writes (0xA0-0xA2) 4 would leave out are sent
//                    behind GXV_ARRAYSKIP=2's markers (the front end's "arrayskip verify:" must count no differing one); 16 (with 8) one in
//                    17 packets' markers carries a wrong value (the check must see them)
//   FEW_DROP=<percent> (lighter25, cost only: the pictures change): that share of the material packets is not drawn (made and thrown away)
//   FEW_MIXED=<n> (feplan31, GXV_FEPLAN): n calls a picture, after the packets, of "mixed" display lists (state commands and draws in one
//                    list, which neither the state-list cache nor the geometry cache takes): per segment a TEV colour (BG three times), a
//                    material colour, the matrix index and a position matrix by an indexed load, then a strip in the shapes' vertex format;
//                    1 to 3 segments; some lists with a CALL_DL inside, state after the last draw, NOPs and a masked write, a vertex-descriptor
//                    write before the first draw (no plan: parsed), an unknown opcode at the end. Half of them are written again every picture
//                    (the same bytes), a quarter animated (a TEV value changes every picture); every 7th picture the scene's vertex format for
//                    their calls is another one (format 1: matrix indices), so that a plan meets a draw size it was not made with.
//                    From a random sequence of their own: the rest of the scene is the same with and without them.
//   FEW_GXDEDUP=<m> (gxdedup31, GXV_GX_DEDUP: platform/include/port/gx_dedup_core.h, the game's decision code itself): the top-level stream
//                    goes through the decision core on its way to the front end, in step with it (an inline draw's size is the front end's
//                    vertex size there): 0 every write sent (the core counts only), 1 the writes it finds repeated left out, 2 sent behind the
//                    game's probes (XF 0x0C00 / 0x0C01: the front end's "gxdedup verify:" must count none differing; GXV_GX_DEDUP_DIGEST=1:
//                    the digest at every probe). A list call forgets what its list may write, by the list's content as the game's callers
//                    vouch for theirs: primitives only (nothing), no CP command and no mask left pending (BP / XF), else everything;
//                    FEW_GXDEDUP_CLASS=0: everything at every call. A port marker (other than the census's and the verify kinds) and the
//                    particle command forget everything. FEW_GXDEDUP_STATE=1: a hash of the front end's State (registers, raw halves, dirty
//                    flags, keys; not its host pointers) at every top-level draw and list call, one "dd-state" line a picture on stdout:
//                    =0 and =1 / =2 must print the same lines. FEW_GXDEDUP_RAND=<n>: n random top-level register writes (repeats, masks,
//                    TEV colour / konst halves, unused arrays, XF colours and matrix indices) after each material list and each shape,
//                    from a sequence of their own, and every EFB copy trigger sent twice. FEW_GXDEDUP_MUTANT=<k>: the core's negative
//                    controls (1 side-effect registers taken as plain, 2 the pending mask ignored, 3 invalidations ignored, 4 every list
//                    taken as primitives only): the pictures or the State must then differ.
//   FEW_TRASH=<KiB>: after each picture, write that much of a buffer of its own (the game's turn and the back end pushing the front end's
//                    tables out of the shared cache between pictures; for a cache simulation: valgrind --tool=callgrind --cache-sim=yes)
#include "vgx_state.h"
#include "vgx_fesplit.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"
#include "vgx_workload.h"
#define PORT_GXDD_TEST_MUTANTS 1
#include "../platform/include/port/gx_dedup_core.h"  // (relative: the suites that build this fixture do not all pass -Iplatform/include)

namespace vgx { void RtReasonCounters(uint64_t out[20]); void RtNoneListText(char* out, uint32_t n, double pics, uint64_t* lost); } // (vgx_dlcache.cpp, GXV_FE_CENSUS: the retained records' reasons and GXV_RT_INST's counters)

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <time.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <thread>

// (FEW_REALTIME=1: the clocks are real, for the time measurements of GXV_FE_TWOCORE; otherwise counters, so that runs repeat exactly)
static bool RealTime() { static const bool on = std::getenv("FEW_REALTIME") && std::atoi(std::getenv("FEW_REALTIME")) != 0; return on; }
extern "C" int64_t port_host_time_ns() {
    if (RealTime()) { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return static_cast<int64_t>(t.tv_sec) * 1000000000 + t.tv_nsec; }
    static std::atomic<int64_t> n{1};
    return n.fetch_add(1000);
}
extern "C" int64_t port_thread_cpu_us() {
    if (RealTime()) { timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return static_cast<int64_t>(t.tv_sec) * 1000000 + t.tv_nsec / 1000; }
    static std::atomic<int64_t> n{1};
    return n.fetch_add(1);
}
extern "C" void port_log_summary(const char* fmt, ...) { // (FEW_LOG=1: on stderr as well, e.g. GXV_FE_CENSUS's lines)
    static const bool on = std::getenv("FEW_LOG") != nullptr;
    if (!on) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}
extern "C" void port_log(const char* fmt, ...) { // (FEW_LOG=1: the front end's log lines on stderr, e.g. GXV_SPLIT=2's verify summary)
    static const bool on = std::getenv("FEW_LOG") != nullptr;
    if (!on) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

namespace vgx {
namespace sc { uint64_t ArrVerifyCounts(uint64_t* bad) __attribute__((weak)); }
uint64_t TevColVerifyCounts(uint64_t* bad) __attribute__((weak)); // (hotprof25: vgx_state.cpp)
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
} // namespace vgx

namespace {
uint64_t g_rng = 0x2545F4914F6CDD1Dull;
uint32_t Rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return static_cast<uint32_t>(g_rng >> 11);
}
uint32_t Rn(uint32_t n) { return Rnd() % n; }
float Rf(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(Rnd() & 0xFFFFFF) / 16777216.0f; }
uint32_t F2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

constexpr uint32_t kRam = 8u << 20;
alignas(4096) uint8_t g_ram[kRam];
uint32_t g_brk = 0x1000;
uint32_t Alloc(uint32_t bytes) {
    const uint32_t a = g_brk;
    g_brk += (bytes + 31u) & ~31u;
    if (g_brk > (6u << 20)) { std::fprintf(stderr, "fixture: guest RAM exhausted\n"); std::exit(2); }
    return a;
}
const uint8_t* Resolve(uint32_t address, uint32_t* avail) {
    *avail = 0;
    address &= 0x3FFFFFFFu; // (cached / uncached mirrors)
    if (address == 0 || address >= kRam) return nullptr;
    *avail = kRam - address;
    return g_ram + address;
}

void Put32(uint8_t* p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i)); }
void PutLE(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); } // (vertex and matrix arrays are host data in this port: little-endian)
void Put16(uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v); }
struct Bytes : std::vector<uint8_t> {
    void U8(uint32_t v) { push_back(static_cast<uint8_t>(v)); }
    void U16(uint32_t v) { const size_t a = size(); resize(a + 2); Put16(data() + a, v); }
    void U32(uint32_t v) { const size_t a = size(); resize(a + 4); Put32(data() + a, v); }
    void BP(uint32_t reg, uint32_t value) { U8(0x61); U32(reg << 24 | (value & 0xFFFFFF)); }
    void Masked(uint32_t reg, uint32_t mask, uint32_t value) { BP(0xFE, mask); BP(reg, value); }
    void CP(uint32_t reg, uint32_t value) { U8(0x08); U8(reg); U32(value); }
    void XF(uint32_t addr, const std::vector<uint32_t>& w) { U8(0x10); U16(static_cast<uint32_t>(w.size() - 1)); U16(addr); for (uint32_t x : w) U32(x); }
    void Call(uint32_t addr, uint32_t size) { U8(0x40); U32(addr); U32(size); }
};
uint32_t Pad(size_t n) { return static_cast<uint32_t>((n + 31) & ~size_t(31)); }
// ---- FEW_GXDEDUP (gxdedup31): see the head of this file
int g_dd = -1, g_ddRand = 0, g_ddClass = 1;
bool g_ddState = false;
PortGxDdShadow g_dds;
uint64_t g_ddW[3], g_ddSame[3], g_ddCalls[3], g_ddMarkers, g_ddFx;
uint64_t g_ddRng = 0x5DEECE66Dull;
uint32_t DdRn(uint32_t n) {
    g_ddRng ^= g_ddRng << 13;
    g_ddRng ^= g_ddRng >> 7;
    g_ddRng ^= g_ddRng << 17;
    return static_cast<uint32_t>(g_ddRng >> 11) % n;
}
// FEW_GXDEDUP_RAND: n random top-level register writes, values from small sets (most writes repeat what a register holds)
void DdRand(Bytes& f) {
    static const uint32_t kVal[4] = {0x000000u, 0x0FF0FFu, 0x1234ABu, 0x7FFFFFu};
    static const uint32_t kMask[3] = {0x0000FFu, 0x00FF00u, 0x0F0F0Fu};
    for (int i = 0; i < g_ddRand; ++i) {
        const uint32_t v = kVal[DdRn(3)];
        switch (DdRn(16)) {
        case 0: case 1: case 2: f.BP(0xE0u + DdRn(8), (DdRn(2) << 23) | (v & 0x7FFFFFu)); break;   // TEV colour / konst halves
        case 3: f.BP(0xC0u + DdRn(4), v); break;                                                   // TEV stages
        case 4: f.BP(0xF3u, 0x3F0000u | (v & 0xFFu)); break;                                        // alpha compare
        case 5: f.Masked(0x41u, kMask[DdRn(3)], v); break;                                          // blend, behind a mask
        case 6: f.Masked(0x30u + DdRn(4), kMask[DdRn(3)], v); break;                                // texture-coordinate scale, behind a mask
        case 7: f.BP(0x40u, DdRn(2) ? 0x17u : 0x07u); break;                                        // depth mode
        case 8: f.CP(0xA5u + DdRn(7), 0x100000u + (v & 0xFFE0u)); break;                            // unused array bases
        case 9: f.CP(0xB5u + DdRn(3), DdRn(2) ? 8u : 12u); break;                                   // and strides
        case 10: f.XF(0x100Au + DdRn(4), {v | 0xFF}); break;                                        // ambient / material colours
        case 11: { const uint32_t a = DdRn(2) ? 0u : 3u; f.XF(0x1018, {a, 0x00F3CF3Cu}); break; }   // both matrix indices, one command
        case 12: f.XF(0x1018, {DdRn(2) ? 0u : 3u}); break;                                          // one
        case 13: f.CP(0x30u, DdRn(2) ? 0u : 3u); break;
        case 14: f.BP(0x28u, DdRn(2) ? 0x3C0u : 0x049u); break;                                     // TEV order
        default: f.BP(0xF2u, v); break;                                                             // fog colour
        }
    }
}
void Store(uint32_t addr, const Bytes& b, uint32_t bytes) {
    if (b.size() > bytes) { std::fprintf(stderr, "fixture: list does not fit\n"); std::exit(2); }
    std::memset(g_ram + addr, 0, bytes);
    std::memcpy(g_ram + addr, b.data(), b.size());
    vgx::hle::NotifyGuestWrite(g_ram + addr, bytes);
}
struct List { uint32_t addr = 0, size = 0; };
// (the lists made again every picture live in memory of their own, as the game's heaps keep them apart from the model data: a
// write tracks 4 KiB pages)
uint32_t g_brkDyn = 6u << 20;
List NewList(const Bytes& b, uint32_t room = 0, bool dyn = false) {
    List l{0, Pad(b.size() + room)};
    if (dyn) { l.addr = g_brkDyn; g_brkDyn += l.size; } else l.addr = Alloc(l.size);
    Store(l.addr, b, l.size);
    return l;
}

// ---- textures: I8 images in guest RAM
constexpr int kTex = 24;
uint32_t g_tex[kTex], g_texW[kTex], g_texH[kTex];

// ---- a J3D material: the values its lists carry
struct Material {
    int texmaps = 1, stages = 1, texgens = 1;
    bool ind = false, texMtx = false;
    uint32_t tex[2] = {}, tevOrder = 0, tevReg[3][2] = {}, konst[4][2] = {}, stage[3][2] = {}, ksel[8] = {}, alpha = 0, blend = 0, zmode = 0,
             fog[6] = {}, matColor = 0, amb = 0, chan[4] = {};
    uint32_t mtx[16] = {};
    // the differed list's values (animated materials change them)
    uint32_t dReg[3][2] = {}, dMtx[12] = {}, dColor = 0;
    bool animated = false;
};
uint32_t Tc(int v) { return static_cast<uint32_t>(v) & 0x7FF; }
uint32_t TevRA(int r, int a, bool k) { return Tc(r) | (Tc(a) << 12) | (k ? 0x800000u : 0u); }
Material NewMaterial() {
    Material m;
    m.texmaps = 1 + (Rn(3) == 0);
    m.texgens = m.texmaps;
    m.stages = 1 + static_cast<int>(Rn(3));
    m.ind = Rn(4) == 0;
    m.texMtx = Rn(3) == 0;
    for (int t = 0; t < m.texmaps; ++t) m.tex[t] = Rn(kTex);
    m.tevOrder = 0x049040u | (Rn(2) << 3);
    const bool oor = Rn(5) == 0; // (TEV registers outside 0..255: GXV_FIX_TEVIN's State::tevOor)
    auto comp = [oor] { return oor && Rn(3) == 0 ? static_cast<int>(Rn(1024)) - 400 : static_cast<int>(Rn(256)); };
    for (int r = 0; r < 3; ++r) {
        m.tevReg[r][0] = TevRA(comp(), comp(), false);
        m.tevReg[r][1] = TevRA(comp(), comp(), false);
    }
    for (int k = 0; k < 4; ++k) {
        m.konst[k][0] = TevRA(static_cast<int>(Rn(256)), 255, true);
        m.konst[k][1] = TevRA(static_cast<int>(Rn(256)), static_cast<int>(Rn(256)), true);
    }
    for (int s = 0; s < 3; ++s) {
        // (colour: d c b a inputs from a small set, alpha likewise; scale / bias / clamp / register 0)
        const uint32_t ci[4] = {Rn(2) ? 0xFu : 8u, Rn(2) ? 8u : 0xAu, Rn(2) ? 0xEu : 0xFu, 0xFu};
        m.stage[s][0] = (ci[0] << 12) | (ci[1] << 8) | (ci[2] << 4) | ci[3] | (1u << 19);
        const uint32_t ai[4] = {Rn(2) ? 7u : 4u, Rn(2) ? 4u : 5u, 7u, 7u};
        m.stage[s][1] = (ai[0] << 13) | (ai[1] << 10) | (ai[2] << 7) | (ai[3] << 4) | (1u << 19);
    }
    for (int i = 0; i < 8; ++i) m.ksel[i] = 0xE338C4u | (i & 1 ? 0xAu : 0u) | (Rn(4) << 14);
    m.alpha = Rn(3) ? 0x770000u : (0x3F0000u | Rn(128));
    m.blend = Rn(3) ? 0x0034A1u : 0x0034A0u | (Rn(2) << 1);
    m.zmode = Rn(4) ? 0x17u : 0x07u;
    m.fog[0] = 0; m.fog[1] = 0x40000Fu; m.fog[2] = 1; m.fog[3] = 0; m.fog[4] = 0xFFFFFF; m.fog[5] = 0x156;
    m.matColor = Rnd() | 0xFFu;
    m.amb = Rnd() | 0xFFu;
    m.chan[0] = Rn(2) ? 0x701u : 0x702u; m.chan[1] = 0x202; m.chan[2] = 0x701; m.chan[3] = 0x400;
    // FEW_LIT=1: lit colour channels (lights 0-2, clamped diffuse, spot attenuation: the game's actor lights), so that GXV_VS_TRIM's light
    // facts reach the keys (the random light-memory writes below toggle them). No extra random numbers: unset, the scene is as before.
    if (std::getenv("FEW_LIT")) { m.chan[0] = (m.chan[0] & 1u) ? 0x71Fu : 0x71Eu; m.chan[2] = 0x71Fu; }
    for (int i = 0; i < 16; ++i) m.mtx[i] = F2u(Rf(-2, 2));
    for (int r = 0; r < 3; ++r) { m.dReg[r][0] = m.tevReg[r][0]; m.dReg[r][1] = m.tevReg[r][1]; }
    for (int i = 0; i < 12; ++i) m.dMtx[i] = F2u(Rf(-1, 1));
    m.dColor = m.matColor;
    m.animated = Rn(8) == 0;
    return m;
}
// The material's display list, in J3DMaterial::makeDisplayList_private's order (J3DTevBlock, J3DIndBlock, J3DPEBlock, gen mode,
// J3DTexGenBlock, J3DColorBlock, channel and texgen counts).
Bytes MaterialList(const Material& m) {
    Bytes b;
    for (int t = 0; t < m.texmaps; ++t) {
        const uint32_t ti = m.tex[t];
        b.BP(0x94 + t, g_tex[ti] >> 5);
        b.BP(0x88 + t, (g_texW[ti] - 1) | ((g_texH[ti] - 1) << 10) | (1u << 20)); // I8
        b.BP(0x80 + t, 0x000195u);
        b.BP(0x84 + t, 0);
    }
    b.BP(0x28, m.tevOrder);
    for (int t = 0; t < m.texgens; ++t) {
        b.Masked(0x30 + 2 * t, 0x03FFFF, g_texW[m.tex[t]] - 1);
        b.Masked(0x31 + 2 * t, 0x03FFFF, g_texH[m.tex[t]] - 1); // (the real lists mask once for both; the same writes)
    }
    for (int r = 0; r < 3; ++r) {
        b.BP(0xE2 + 2 * r, m.tevReg[r][0]);
        for (int k = 0; k < 3; ++k) b.BP(0xE3 + 2 * r, m.tevReg[r][1]); // (BG three times: the hardware's write delay)
    }
    for (int k = 0; k < 4; ++k) { b.BP(0xE0 + 2 * k, m.konst[k][0]); b.BP(0xE1 + 2 * k, m.konst[k][1]); }
    for (int s = 0; s < m.stages; ++s) {
        b.BP(0xC0 + 2 * s, m.stage[s][0]);
        b.BP(0xC1 + 2 * s, m.stage[s][1]);
        b.BP(0x10 + s, 0);
    }
    for (int i = 0; i < 8; ++i) b.BP(0xF6 + i, m.ksel[i]);
    if (m.ind) { b.BP(0x06, 0x200); b.BP(0x07, 0x100000); b.BP(0x08, 0x400000); b.BP(0x25, 0); }
    for (int i = 0; i < 3; ++i) { b.Masked(0x2E, 0x03FFFF, 0); b.BP(0x2F, 0); }
    b.BP(0x27, 0xFFFFC9);
    b.BP(0x0F, 2);
    for (int i = 0; i < 5; ++i) b.BP(0xEE + i, m.fog[i]);
    b.BP(0xE8, m.fog[5]);
    b.BP(0xF3, m.alpha);
    b.Masked(0x41, 0x00FFE7, m.blend);
    // (twocore24, FEW_ALPHA=1: every third material writes alpha, so that GXV_SHADOWFILL_BOUNDS / GXV_SHADOW_RECT work out alpha bounds for
    // its static runs (the console's default); chosen from the material's colour, no random number drawn: unset, the scene is as before)
    static const bool alphaMats = std::getenv("FEW_ALPHA") && std::atoi(std::getenv("FEW_ALPHA")) != 0;
    if (alphaMats && (m.matColor >> 8) % 3 == 0) {
        b.Masked(0x43, 0x000007, 0x000001); // (an EFB with alpha: RGBA6_Z24)
        b.Masked(0x41, 0x000018, 0x000018);
    }
    b.BP(0x40, m.zmode);
    b.Masked(0x43, 0x000040, 0x40);
    // (FEW_CULL=1, farlod32 / GXV_FACECULL: the materials cull back faces, every third one front faces, as the game's map materials do; default: none, as before)
    static const bool cullOn = std::getenv("FEW_CULL") != nullptr;
    b.Masked(0x00, 0x07FC3F, static_cast<uint32_t>(m.texgens) | (1u << 4) | (static_cast<uint32_t>(m.stages - 1) << 10) | (m.ind ? 1u << 16 : 0u) |
                                 (cullOn ? (m.stages % 3 == 0 ? 2u : 1u) << 14 : 0u));
    if (m.texMtx) {
        b.XF(0x78, std::vector<uint32_t>(m.mtx, m.mtx + 8));
        b.XF(0x84, std::vector<uint32_t>(m.mtx + 8, m.mtx + 16));
    }
    std::vector<uint32_t> tg, pt;
    for (int t = 0; t < m.texgens; ++t) { tg.push_back(0x5280); pt.push_back(0x3D); }
    b.XF(0x1040, tg);
    b.XF(0x1050, pt);
    b.XF(0x100C, {m.matColor, m.matColor});
    b.XF(0x100A, {m.amb, 0});
    b.XF(0x100E, {m.chan[0], m.chan[1], m.chan[2], m.chan[3]});
    b.XF(0x1009, {1});
    b.XF(0x103F, {static_cast<uint32_t>(m.texgens)});
    return b;
}
// The differed list of the packet (J3DShapePacket's, made again from the animated values on every drawn turn).
int g_konstDiff = 0; // (FEW_KONST=1: the differed lists of every other material carry J3DDiffFlag_KonstColor's part, as J3DMaterial::diff
                     // makes it: a konstant colour (BP 0xE0+ with the konst bit) and J3DGDSetGenMode_3Param, the GenMode write behind a mask)
int g_tinyDiff = 0, g_tinyGeo = 0, g_bypassGeo = 0; // (FEW_TINYDIFF, FEW_TINYGEO, FEW_BYPASS: see the head of the file)
uint64_t g_csRng = 0x6A09E667F3BCC909ull;
uint32_t CsRn(uint32_t n) { g_csRng ^= g_csRng << 13; g_csRng ^= g_csRng >> 7; g_csRng ^= g_csRng << 17; return static_cast<uint32_t>(g_csRng >> 11) % n; }
Bytes DiffList(const Material& m, int kind = 0) {
    Bytes b;
    if (kind == 1) { b.XF(0x100C, {m.dColor}); return b; }                                                    // (13 bytes: 32)
    if (kind == 2) { b.XF(0x1D * 3 + 0, std::vector<uint32_t>(m.dMtx, m.dMtx + 12)); b.XF(0x100C, {m.dColor}); return b; } // (62 bytes: 64)
    if (g_konstDiff && (m.dReg[0][0] & 1u)) {
        b.BP(0xE0, m.dReg[0][0] | 0x800000u);
        b.BP(0xE1, m.dReg[0][1] | 0x800000u);
        b.Masked(0x00, 0x073C0F, static_cast<uint32_t>(m.texgens) | (static_cast<uint32_t>(m.stages - 1) << 10) | (m.ind ? 1u << 16 : 0u));
    }
    for (int r = 0; r < 3; ++r) {
        b.BP(0xE2 + 2 * r, m.dReg[r][0]);
        for (int k = 0; k < 3; ++k) b.BP(0xE3 + 2 * r, m.dReg[r][1]);
    }
    b.XF(0x1D * 3 + 0, std::vector<uint32_t>(m.dMtx, m.dMtx + 12)); // (a texture matrix in position matrix memory, row 0x1D)
    b.XF(0x100C, {m.dColor});
    return b;
}
uint32_t g_pos = 0, g_nrm = 0, g_uv = 0;
Bytes VtxFmt(int kind) {
    Bytes b;
    static const bool j3dFmt = std::getenv("FEW_J3DFMT") && std::atoi(std::getenv("FEW_J3DFMT")) == 1;
    if (j3dFmt) { b.CP(0xA0, g_pos); b.CP(0xB0, 12); b.CP(0xA1, g_nrm); b.CP(0xB1, 12); b.CP(0xA4, g_uv); b.CP(0xB4, 8); } // (FEW_J3DFMT: as the shapes write them)
    uint32_t vcd = 3u << 9 | 3u << 11; // POS, NRM index16
    if (kind == 1) vcd |= 1u;          // PNMTXIDX
    b.CP(0x50, vcd);
    b.CP(0x60, 3u); // TEX0 index16
    // FEW_S16=1 (geofeed23, GXV_VTXPACK): positions s16 >> 4, normals s16 >> 14, tex0 s16 >> 8 (the same array bytes read as fixed point:
    // other numbers, the same for every run), so that the cache's static runs are packed; otherwise floats, as the cooked data has them
    static const bool s16 = std::getenv("FEW_S16") && std::atoi(std::getenv("FEW_S16")) == 1;
    if (s16) b.CP(0x70, 1u | (3u << 1) | (4u << 4) | (3u << 10) | (1u << 21) | (3u << 22) | (8u << 25));
    else b.CP(0x70, 1u | (4u << 1) | (4u << 10) | (1u << 21) | (4u << 22));
    b.CP(0x80, 0);
    b.CP(0x90, 0);
    b.XF(0x1008, {1u | (1u << 2) | (1u << 4)});
    return b;
}
constexpr uint32_t kVerts = 200;
Bytes Geometry(int kind, uint32_t prims) {
    Bytes b;
    for (uint32_t p = 0; p < prims; ++p) {
        const uint32_t n = 4 + Rn(14);
        b.U8(0x98); // triangle strip, format 0
        b.U16(n);
        for (uint32_t i = 0; i < n; ++i) {
            if (kind == 1) b.U8(Rn(4) * 3);
            b.U16(Rn(kVerts));
            b.U16(Rn(kVerts));
            b.U16(Rn(kVerts));
        }
    }
    return b;
}

uint32_t g_mtxArr = 0;

// ---- odd lists (FEW_ODD=0: none): state lists of random shape, to exercise every branch of the register-delta compiler and
// replay (GXV_SLDELTA): registers written again with the same or another value, behind masks, masks before TEV / texture registers or
// at the end, TEV halves outside 0..255, the texture units' LOD registers, XF registers, matrix indices through XF and CP, overlapping
// matrix-memory blocks, light memory, CP arrays and vertex formats, an unknown CP register, an indexed load, a PE-done write.
uint32_t OddVal(uint32_t reg, uint32_t salt) { return ((reg * 0x9E3779B1u) ^ (salt * 0x85EBCA6Bu)) & 0x00FFFFFFu; }
Bytes OddList() {
    static const uint8_t plain[] = {0x0F, 0x10, 0x11, 0x20, 0x21, 0x22, 0x25, 0x26, 0x27, 0x28, 0x30, 0x31, 0x32, 0x33, 0x40, 0x41, 0x42, 0x43,
                                    0x59, 0xC0, 0xC1, 0xC2, 0xC3, 0xEE, 0xEF, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF6, 0xF7, 0x06, 0x07, 0x08};
    static const uint8_t tex[] = {0x80, 0x81, 0x84, 0x85, 0x88, 0x94, 0xA0, 0xA4, 0xA8};
    static const uint16_t xfr[] = {0x1009, 0x100A, 0x100B, 0x100C, 0x100D, 0x100E, 0x100F, 0x1010, 0x1011, 0x1012, 0x1040, 0x1041, 0x1050, 0x1051, 0x103F, 0x101C};
    Bytes b;
    const uint32_t n = 4 + Rn(28);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t k = Rn(16);
        if (k < 4) {
            const uint32_t r = plain[Rn(sizeof plain)];
            uint32_t v = OddVal(r, Rn(2));
            if (r == 0x28) v &= 0x3C7C7u; // (TEV order: texture maps 0..3 or none)
            if (r == 0x41) v &= 0x00FFF7u;
            b.BP(r, v);
        } else if (k == 4) {
            const uint32_t r = plain[Rn(sizeof plain)];
            b.Masked(r, Rn(2) ? 0x00FF00u : 0x03FFFFu, OddVal(r, Rn(2)));
        } else if (k == 5 && Rn(4) == 0) {
            b.BP(0xFE, 0x000FFFu); // (a mask before a TEV or texture register: not compiled)
            b.BP(Rn(2) ? 0xE2 : 0x80, OddVal(0xE2, 0) & 0x7FF7FFu);
        } else if (k == 6) {
            const uint32_t r = tex[Rn(sizeof tex)];
            uint32_t v = OddVal(r, Rn(2));
            if (r == 0x94 || r == 0x88) v = r == 0x94 ? (g_tex[Rn(2)] >> 5) : ((g_texW[0] - 1) | ((g_texH[0] - 1) << 10) | (1u << 20));
            b.BP(r, v);
        } else if (k == 7 || k == 8) {
            const uint32_t r = 0xE0 + Rn(8);
            const int lo = static_cast<int>(Rn(3) == 0 ? Rn(2048) : Rn(256)), hi = static_cast<int>(Rn(3) == 0 ? Rn(2048) : Rn(256));
            b.BP(r, (static_cast<uint32_t>(lo) & 0x7FF) | ((static_cast<uint32_t>(hi) & 0x7FF) << 12) | (Rn(2) ? 0x800000u : 0u));
        } else if (k == 9) {
            const uint32_t a = xfr[Rn(sizeof xfr / sizeof xfr[0])], c = 1 + Rn(3);
            std::vector<uint32_t> w;
            for (uint32_t j = 0; j < c && a + j < 0x1058; ++j) w.push_back(a == 0x101C ? F2u(16777215.0f - static_cast<float>(Rn(2))) : (Rn(2) ? 0x701u : 0x202u) + j);
            b.XF(a, w);
        } else if (k == 10) {
            if (Rn(2)) b.XF(0x1018, {0x3CF3CF00u | Rn(3) * 3});
            else b.XF(0x1017, {0, 0x3CF3CF00u, 0x00F3CF3Cu});
        } else if (k == 11) {
            const uint32_t region = Rn(4);
            const uint32_t base = region == 0 ? 0x30 + Rn(0x10) : region == 1 ? 0x400 + Rn(0x20) : region == 2 ? 0x500 + Rn(0x30) : 0x600 + Rn(0x40);
            const uint32_t c = 1 + Rn(region == 3 ? 16 : 12);
            std::vector<uint32_t> w;
            for (uint32_t j = 0; j < c; ++j) w.push_back(F2u(static_cast<float>(Rn(3)) * 0.5f));
            b.XF(base, w);
        } else if (k == 12) {
            const uint32_t c = Rn(5);
            if (c == 0) b.CP(Rn(2) ? 0x30 : 0x40, Rn(2) ? 0u : 3u);
            else if (c == 1) b.CP(0xA0 + Rn(2), Rn(2) ? g_pos : g_nrm);
            else if (c == 2) b.CP(0xB0, 12);
            else if (c == 3) { const Bytes f = VtxFmt(static_cast<int>(Rn(2))); b.insert(b.end(), f.begin(), f.begin() + 6 * (1 + Rn(5))); }
            else if (Rn(4) == 0) b.CP(0x20, 1); // (a CP register the delta does not take)
        } else if (k == 13 && Rn(3) == 0) {
            const uint32_t c = Rn(3);
            if (c == 0) { b.U8(0x20); b.U16(Rn(16)); b.U16((11u << 12) | 0x30); } // (an indexed load: not compiled)
            else if (c == 1) b.BP(0x45, 0); // (PE done without the done bit: an effect register)
            else b.U8(0x44);
        }
    }
    if (Rn(16) == 0) b.BP(0xFE, 0x00FFFFu); // (a mask at the end: applies to the caller's next write)
    return b;
}
std::vector<List> g_odd;
bool g_oddOn = true;

// ---- mixed lists (FEW_MIXED, feplan31)
int g_mixedN = 0;
uint64_t g_mxRng = 0xBB67AE8584CAA73Bull;
uint32_t MxRn(uint32_t n) { g_mxRng ^= g_mxRng << 13; g_mxRng ^= g_mxRng >> 7; g_mxRng ^= g_mxRng << 17; return static_cast<uint32_t>(g_mxRng >> 11) % n; }
struct Mixed { List l; int kind = 0; bool dyn = false, animated = false; uint32_t seed = 0; };
std::vector<Mixed> g_mixed;
Bytes MixedList(const Mixed& m, unsigned frame) {
    uint64_t r = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(m.seed) * 0xD1B54A32D192ED03ull); // (the list's own bytes, the same every call)
    auto rn = [&r](uint32_t n) { r ^= r << 13; r ^= r >> 7; r ^= r << 17; return static_cast<uint32_t>(r >> 11) % n; };
    Bytes b;
    const uint32_t segs = 1 + rn(3);
    if (m.kind == 3) b.CP(0x50, 3u << 9 | 3u << 11); // (the vertex descriptor written before a draw: no plan)
    for (uint32_t sgi = 0; sgi < segs; ++sgi) {
        const uint32_t anim = m.animated && sgi == 0 ? frame * 7u : 0u;
        b.BP(0xE2, TevRA(static_cast<int>((rn(256) + anim) & 255), 255, false));
        for (int k = 0; k < 3; ++k) b.BP(0xE3, TevRA(static_cast<int>(rn(256)), static_cast<int>(rn(256)), false));
        if (m.kind == 4) { b.U8(0x44); b.Masked(0x41, 0x00FFE7, 0x0034A1u); b.U8(0x48); }
        b.XF(0x100C, {rn(0xFFFFFF) << 8 | 0xFFu});
        b.CP(0x30, 0);
        b.U8(0x20); b.U16(rn(16)); b.U16((11u << 12) | 0);
        if (m.kind == 1 && sgi == 0) b.Call(g_mixed.empty() ? 0x20u : g_mixed[0].l.addr, 64); // (a CALL_DL inside a list: ignored)
        const uint32_t n = 3 + rn(10);
        b.U8(0x98); b.U16(n);
        for (uint32_t i = 0; i < n; ++i) { b.U16(rn(kVerts)); b.U16(rn(kVerts)); b.U16(rn(kVerts)); }
    }
    if (m.kind == 2) { b.BP(0xF3, 0x3F0000u | rn(128)); b.XF(0x100C, {0xFFFFFFFFu}); }
    if (m.kind == 5) b.U8(0x02); // (an opcode the parser does not know: it stops there)
    return b;
}
void BuildMixed() {
    for (int i = 0; i < 16; ++i) {
        Mixed m;
        m.kind = static_cast<int>(MxRn(6));
        m.dyn = MxRn(2) == 0;
        m.animated = MxRn(4) == 0;
        m.seed = MxRn(1u << 30);
        const Bytes b = MixedList(m, 0);
        m.l = NewList(b, 64, m.dyn);
        g_mixed.push_back(m);
    }
}
void MutateMixed(unsigned frame) {
    for (Mixed& m : g_mixed) {
        if (!m.dyn && !m.animated) continue;
        Store(m.l.addr, MixedList(m, frame), m.l.size); // (written again: the same bytes, or an animated value)
    }
}

struct Packet {
    int mat;
    List matList;
    List diff[2];
    int cur = 0;
    uint32_t matId = 0, diffId = 0, matVer = 1, diffVer = 1, memberVer = 1, diffKinds = 0; // (FEW_J3N: what the game's J3DDisplayListObj keeps)
    struct Shape { int kind; List geo; bool idxMtx; uint32_t handle = 0; int geoKind = 0; }; // (geoKind, FEW_CONSOLE: 1 tiny, 2 rewritten)
    int diffKind = 0; // (FEW_CONSOLE: 0 the fixture's differed list, 1 / 2 J3D's small ones)
    std::vector<Shape> shapes;
    bool visible = true;
};
std::vector<Material> g_mats;
std::vector<Packet> g_pk;
// ---- FEW_J3N (GXV_J3DNATIVE)
bool g_j3n = false, g_diffKeep = false, g_miss = false, g_sceneReuse = false;
int g_lightEvery = 0, g_fogEvery = 0, g_sceneEvery = 0;
// (j3n2) FEW_CAM=1: the camera moves (the projection at top level changes every picture); FEW_COPY=n: an EFB copy to a texture after every
// n-th drawer (mid-run, between a drawer's shapes and the next one's material list)
int g_cam = 0, g_copyEvery = 0;
uint32_t g_copyDst = 0;
uint32_t g_extLight = 1, g_extFog = 1, g_extScene = 1, g_idCounter = 0;
uint32_t J3nExt() { return g_extScene * 1000003u + g_extLight * 131u + g_extFog * 7u; }
// One call, bracketed as the game's port_gx_call_bracketed sends it: 0x5A, five words, the call, 0x5B (or the plain call).
void CallBracket(Bytes& f, const List& l, uint32_t id, uint32_t verA, uint32_t verB, uint32_t flags) {
    if (!g_j3n) { f.Call(l.addr, l.size); return; }
    // (j3n2: the game's id is the list's address (DrawBuffer.cpp drawPort), 32-byte aligned; FEW_J3N_ADDRID=0: the holder's number, as before)
    static const bool addrId = !(std::getenv("FEW_J3N_ADDRID") && std::atoi(std::getenv("FEW_J3N_ADDRID")) == 0);
    if (addrId) id = l.addr;
    f.U8(0x5A); f.U32(id); f.U32(verA); f.U32(verB); f.U32(J3nExt()); f.U32(flags);
    f.Call(l.addr, l.size);
    f.U8(0x5B);
}
List g_fmt[2];
// FEW_FX=<n> (default 0: none, and nothing else changes): n particle-like quads at the end of every picture, as the game's effects come:
// two blended materials (one depth-tested, one not), then per quad its position matrix, a TEV colour and a tiny display list of one
// quad (indexed position and texture coordinate, a direct colour: 27 bytes), so the dynamic-draw path (decode, CPU transform, fold,
// cover bounds, records) is measured too.
int g_fxCount = 0;
int g_shStatic = 0, g_shDyn = 0; // FEW_SHADOW, FEW_SHADOWDYN
List g_shMat, g_shFmt, g_shGeo;
uint32_t g_shHandle = 0;
uint64_t g_shRng = 0x7A1B2C3D4E5F6071ull;
uint32_t ShRn(uint32_t n) { g_shRng ^= g_shRng << 13; g_shRng ^= g_shRng >> 7; g_shRng ^= g_shRng << 17; return static_cast<uint32_t>(g_shRng >> 11) % n; }
float ShRf(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(ShRn(1u << 20)) / static_cast<float>(1u << 20); }
List g_fxMat[2], g_fxFmt;
std::vector<List> g_fxQuads;
// FEW_JPA=<n> (default 0: none, and nothing else changes): n particles per picture in the command order of JPAEmitterManager::draw,
// JPAResource::draw / setPTev / JPABaseShape::setGX and the per-particle functions (JPARegistPrmAlpha(Env), JPALoadCalcTexCrdMtxAnm,
// JPADrawBillboard / JPADrawRotBillboard): emitters of 1-40 particles, each with its immediate set-up (blend, z, alpha compare, TEV
// stage 0 colour / alpha inputs reading C0 / C1 and the texture, no colour channel, one texgen, its texture, the arrays jpa_pos / jpa_crd
// at the emitter's offsets), then per particle its TEV register 0 (and 1), for some emitters a 2x4 texture matrix (TEXMTX0), the position
// matrix (PNMTX0) and GXCallDisplayList(jpa_dl, 32): one quad, POS / TEX0 index8, s8 (VTXFMT0). Some emitters are depth-tested, some not
// (the cover bounds), some have the "clip on" flush primitive in front of every call (GX_MT_XF_FLUSH: an all-zero triangle strip).
int g_jpaCount = 0;
unsigned g_frame = 0;
int g_census = 0; // (FEW_CENSUS)
bool CensusPic() { return g_census > 0 && g_frame % static_cast<unsigned>(g_census) == 0; }
void CMark(Bytes& f, uint32_t kind, uint32_t payload) { if (CensusPic()) f.BP(0xFF, kind | (payload << 8)); }
List g_jpaDl, g_jpaPos, g_jpaCrd;
struct JpaEmitter { int n; int tex; bool zTest, env, texAnm, rot, flush, blendAdd; int posOff, crdOff; uint32_t prm, envc; int var; };
std::vector<JpaEmitter> g_jpaEm;
// FEW_FXMERGE=1 (default 0: the separate commands): the particles' calls as the game's GX library gathers them with GXV_FXMERGE=1
// (decomp/src/RVL_SDK/gx/GXDisplayList.c): per emitter loop one or more commands 0x58 (vgx_state.cpp FxMergeRun) holding the same
// TEV colour, XF and display-list call items, the flush primitive as a count on its call. The command buffers must be the same.
bool g_fxMerge = false;
// FEW_JPAVAR=1 (default 0: none, and nothing else changes): emitters of other kinds as well, so that every way of an item is taken:
// (var 1) the eight-vertex list of JPADrawRotation's jpa_dl_x; (2) a list with a BP write in front of the quad (CallList's way);
// (3) a 96-byte quad list (the geometry cache's way); (4) a post-transform matrix load (XF 0x500) per particle; (5) a 3x4 texture
// matrix with a texgen from the position (a projected texture: not transformed on the CPU); (6) GXSetTevColor of register 2 too;
// (7) a draw-only list made of two quads (not one draw: ProcessStream's way); (8) a command cut into several (the 8 KiB cap).
bool g_jpaVar = false;
// FEW_FUR (see the head of the file; 0: none, and nothing else changes)
int g_furLayers = 0, g_furCount = 6;
int g_plants = 0; // FEW_PLANTS
List g_plantGeo, g_plantMat;
int g_inst = 0; // FEW_INST
List g_instGeo, g_instMat;
uint32_t g_instHandle = 0;
std::vector<uint32_t> g_instPos, g_instNrm;
uint64_t g_instRng = 0x51A7C0DE12345679ull;
uint32_t InstRn(uint32_t n) { g_instRng ^= g_instRng << 13; g_instRng ^= g_instRng >> 7; g_instRng ^= g_instRng << 17; return static_cast<uint32_t>(g_instRng >> 11) % n; }
float InstRf(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(InstRn(1u << 20)) / static_cast<float>(1u << 20); }
List g_furGeo, g_furMat;
std::vector<uint32_t> g_furPos; // [0] the model's own positions, [1 + i] layer i's
// FEW_RETAINED (see the head of the file): 0 none, 1 the shapes' handles, 2 the handles with some wrong ones
int g_retained = -1;
uint64_t g_rtRng = 0x51ED27F1A3C4B5D9ull;
uint32_t RtRn(uint32_t n) { g_rtRng ^= g_rtRng << 13; g_rtRng ^= g_rtRng >> 7; g_rtRng ^= g_rtRng << 17; return static_cast<uint32_t>(g_rtRng >> 11) % n; }
List g_jpaDlX, g_jpaDlBp, g_jpaDlBig, g_jpaDl2;

// ---- FEW_BIG: large streamed draws (their own random sequence)
int g_bigCount = 0;
uint64_t g_bigRng = 0x51ED2701A3C4E5B7ull;
uint32_t BRn(uint32_t n) {
    g_bigRng ^= g_bigRng << 13;
    g_bigRng ^= g_bigRng >> 7;
    g_bigRng ^= g_bigRng << 17;
    return static_cast<uint32_t>(g_bigRng >> 11) % n;
}
float BRf(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(BRn(1u << 24)) / 16777216.0f; }
std::vector<List> g_bigLists; // (written again every picture)
// One draw: kind 0 direct (format 2: XYZ s16 / 64, RGBA8, ST u16 / 256), 1 indexed (format 3: POS, NRM f32 from the scene's arrays by 16- /
// 8-bit indices, RGBA8 direct, ST f32 by a 16-bit index), 2 kind 0 with PNMTXIDX (rows 0, 3, 6), 4 direct 8-bit (format 4: XYZ s8 / 4,
// RGBA8, ST u8 / 8: the decode's byte tables).
void BigDraw(Bytes& b, int kind) {
    static const uint8_t prims[4] = {0x98, 0x90, 0x80, 0xA8}; // strip, triangles, quads, lines
    const uint8_t prim = prims[BRn(10) < 7 ? 0 : 1 + BRn(3)];
    uint32_t n = BRn(8) == 0 ? 3 + BRn(14) : 17 + BRn(260);
    if (prim == 0x90) n = n / 3 * 3 + 3;
    if (prim == 0x80) n = n / 4 * 4 + 4;
    if (prim == 0xA8) n = n / 2 * 2 + 2;
    const uint32_t fmt = kind == 1 ? 3 : kind == 4 ? 4 : 2;
    b.U8(prim | fmt);
    b.U16(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (kind == 1) {
            b.U16(BRn(kVerts));
            b.U8(BRn(kVerts));
            b.U32(BRn(0xFFFFFF) << 8 | 0xFF);
            b.U16(BRn(kVerts));
        } else if (kind == 4) {
            for (int c = 0; c < 3; ++c) b.U8(BRn(256));
            b.U32(BRn(0xFFFFFF) << 8 | 0xFF);
            b.U8(BRn(256));
            b.U8(BRn(256));
        } else {
            if (kind == 2) b.U8(BRn(3) * 3);
            for (int c = 0; c < 3; ++c) b.U16(static_cast<uint32_t>(static_cast<int32_t>(BRf(-200, 200))) & 0xFFFF);
            b.U32(BRn(0xFFFFFF) << 8 | (0x80 + BRn(0x80)));
            b.U16(BRn(512));
            b.U16(BRn(512));
        }
    }
}
void BigFormat(Bytes& f, int kind) {
    if (kind == 1) {
        f.CP(0x50, (3u << 9) | (2u << 11) | (1u << 13)); // POS index16, NRM index8, CLR0 direct
        f.CP(0x60, 3u);                                  // TEX0 index16
        f.CP(0x73, 1u | (4u << 1) | (4u << 10) | (1u << 13) | (5u << 14) | (1u << 21) | (4u << 22));
        f.CP(0x83, 0);
        f.CP(0x93, 0);
        f.CP(0xA0, g_pos); f.CP(0xB0, 12);
        f.CP(0xA1, g_nrm); f.CP(0xB1, 12);
        f.CP(0xA4, g_uv); f.CP(0xB4, 8);
        f.XF(0x1008, {1u | (1u << 2) | (1u << 4)});
    } else if (kind == 4) {
        f.CP(0x50, (1u << 9) | (1u << 13));
        f.CP(0x60, 1u);
        f.CP(0x74, 1u | (1u << 1) | (2u << 4) | (1u << 13) | (5u << 14) | (1u << 21) | (0u << 22) | (3u << 25));
        f.CP(0x84, 0);
        f.CP(0x94, 0);
        f.XF(0x1008, {1u | (1u << 4)});
    } else {
        f.CP(0x50, (kind == 2 ? 1u : 0u) | (1u << 9) | (1u << 13)); // (PNMTXIDX,) POS direct, CLR0 direct
        f.CP(0x60, 1u);                                             // TEX0 direct
        f.CP(0x72, 1u | (3u << 1) | (6u << 4) | (1u << 13) | (5u << 14) | (1u << 21) | (2u << 22) | (8u << 25));
        f.CP(0x82, 0);
        f.CP(0x92, 0);
        f.XF(0x1008, {1u | (1u << 4)});
    }
}
void BigFrame(Bytes& f) {
    if (g_bigLists.empty()) {
        Bytes nop;
        nop.U8(0); // (a list of padding, filled each picture: one per draw)
        for (int i = 0; i < g_bigCount; ++i) g_bigLists.push_back(NewList(nop, 12u << 10, true));
    }
    for (int r = 0; r < 3; ++r) { // (position matrices 0, 3, 6)
        std::vector<uint32_t> w;
        for (int i = 0; i < 12; ++i) w.push_back(F2u((i % 5) == 0 ? BRf(0.004f, 0.01f) : BRf(-0.002f, 0.002f)));
        w[3] = F2u(BRf(-3, 3)); w[7] = F2u(BRf(-2, 2)); w[11] = F2u(BRf(-30, -8));
        f.XF(static_cast<uint32_t>(r * 12), w);
    }
    f.CP(0x30, 0);
    for (int i = 0; i < g_bigCount; ++i) {
        const Packet& k = g_pk[BRn(static_cast<uint32_t>(g_pk.size()))];
        f.Call(k.matList.addr, k.matList.size);
        const uint32_t pick = BRn(10);
        const int kind = pick < 3 ? 0 : pick < 6 ? 1 : pick < 7 ? 2 : pick < 9 ? 3 : 4;
        BigFormat(f, kind == 3 ? 0 : kind);
        if (kind == 3) {
            // (a list made again every picture: the cache gives up on it and parses it as a stream)
            const List& l = g_bigLists[static_cast<size_t>(i)];
            Bytes b;
            const uint32_t draws = 1 + BRn(2);
            for (uint32_t d = 0; d < draws; ++d) BigDraw(b, 0);
            Store(l.addr, b, l.size);
            f.Call(l.addr, Pad(b.size()));
        } else {
            BigDraw(f, kind);
        }
        if (BRn(16) == 0) f.BP(0x48, BRn(0x10000)); // (a draw-sync token: the decodes sent so far settle)
    }
}

void Build() {
    for (int t = 0; t < kTex; ++t) {
        g_texW[t] = 8u << Rn(3);
        g_texH[t] = 8u << Rn(3);
        const uint32_t bytes = g_texW[t] * g_texH[t];
        g_tex[t] = Alloc(bytes);
        for (uint32_t i = 0; i < bytes; ++i) g_ram[g_tex[t] + i] = static_cast<uint8_t>(Rnd());
        vgx::hle::NotifyGuestWrite(g_ram + g_tex[t], bytes);
    }
    g_pos = Alloc(kVerts * 12);
    g_nrm = Alloc(kVerts * 12);
    g_uv = Alloc(kVerts * 8);
    for (uint32_t i = 0; i < kVerts * 3; ++i) { PutLE(g_ram + g_pos + i * 4, F2u(Rf(-1, 1))); PutLE(g_ram + g_nrm + i * 4, F2u(Rf(-1, 1))); }
    for (uint32_t i = 0; i < kVerts * 2; ++i) PutLE(g_ram + g_uv + i * 4, F2u(Rf(0, 1)));
    vgx::hle::NotifyGuestWrite(g_ram + g_pos, kVerts * 12);
    vgx::hle::NotifyGuestWrite(g_ram + g_nrm, kVerts * 12);
    vgx::hle::NotifyGuestWrite(g_ram + g_uv, kVerts * 8);
    g_mtxArr = Alloc(16 * 48);
    if (g_copyEvery > 0) g_copyDst = Alloc(64 * 64 * 4);
    for (uint32_t i = 0; i < 16 * 12; ++i) {
        const uint32_t k = i % 12;
        const float v = k == 3 ? Rf(-5, 5) : k == 7 ? Rf(-4, 4) : k == 11 ? Rf(-40, -6) : (k % 5) == 0 ? Rf(0.5f, 2) : Rf(-0.5f, 0.5f);
        PutLE(g_ram + g_mtxArr + i * 4, F2u(v));
    }
    vgx::hle::NotifyGuestWrite(g_ram + g_mtxArr, 16 * 48);
    for (int k = 0; k < 2; ++k) g_fmt[k] = NewList(VtxFmt(k), 0xC0 - Pad(VtxFmt(k).size()));
    for (int i = 0; i < 40; ++i) g_mats.push_back(NewMaterial());
    for (int i = 0; i < 12 && g_oddOn; ++i) g_odd.push_back(NewList(OddList(), 256));
    const int nPackets = std::getenv("FEW_PACKETS") ? std::atoi(std::getenv("FEW_PACKETS")) : 110; // (fe2-26: FEW_PACKETS=250 is about the console's Honeyhive picture: ~260 material lists, ~540 shape lists)
    for (int p = 0; p < nPackets; ++p) {
        Packet k;
        k.mat = static_cast<int>(Rn(static_cast<uint32_t>(g_mats.size())));
        k.matList = NewList(MaterialList(g_mats[k.mat]));
        k.matId = ++g_idCounter;
        k.diffId = ++g_idCounter;
        if (g_tinyDiff > 0 && static_cast<int>(CsRn(8)) < g_tinyDiff) k.diffKind = 1 + static_cast<int>(CsRn(2));
        const Bytes d = DiffList(g_mats[k.mat], k.diffKind);
        k.diff[0] = NewList(d, 32, true);
        k.diff[1] = NewList(d, 32, true);
        const int n = 1 + static_cast<int>(Rn(3));
        for (int s = 0; s < n; ++s) {
            Packet::Shape sh;
            sh.kind = Rn(5) == 0 ? 1 : 0;
            sh.geo = NewList(Geometry(sh.kind, 6 + Rn(20)));
            if (g_tinyGeo > 0 && static_cast<int>(CsRn(24)) < g_tinyGeo) {
                sh.geoKind = 1; // (a small shape: one strip of 4..6 vertices, 64 bytes at most)
                Bytes t;
                const uint32_t n = 4 + CsRn(3);
                t.U8(0x98); t.U16(n);
                for (uint32_t i = 0; i < n; ++i) { if (sh.kind == 1) t.U8(CsRn(4) * 3); t.U16(CsRn(kVerts)); t.U16(CsRn(kVerts)); t.U16(CsRn(kVerts)); }
                sh.geo = NewList(t);
            } else if (g_bypassGeo > 0 && static_cast<int>(CsRn(32)) < g_bypassGeo) {
                sh.geoKind = 2; // (written again every picture with other indices: Mutate)
                sh.geo = NewList(Geometry(sh.kind, 6 + CsRn(10)), 0, true);
            }
            sh.idxMtx = Rn(4) == 0;
            static uint32_t handles = 0;
            sh.handle = ++handles; // (as J3DShapeDraw's: one number per shape, never another's)
            k.shapes.push_back(sh);
        }
        g_pk.push_back(k);
    }
    if (g_fxCount > 0) {
        for (int i = 0; i < 2; ++i) {
            Material m = NewMaterial();
            m.blend = 0x0034A1u;                // blend on
            m.zmode = i == 0 ? 0x07u : 0x00u;   // depth-tested without update; not depth-tested
            m.texMtx = false;
            g_mats.push_back(m);
            g_fxMat[i] = NewList(MaterialList(m));
        }
        Bytes v;
        v.CP(0x50, (2u << 9) | (1u << 13));  // POS index8, CLR0 direct
        v.CP(0x60, 2u);                      // TEX0 index8
        v.CP(0x71, 1u | (4u << 1) | (1u << 13) | (5u << 14) | (1u << 21) | (4u << 22)); // fmt 1: XYZ f32, RGBA8, ST f32
        v.CP(0x81, 0);
        v.CP(0x91, 0);
        v.XF(0x1008, {1u | (1u << 4)});
        g_fxFmt = NewList(v, 0xC0 - Pad(v.size()));
        for (int q = 0; q < 16; ++q) {
            Bytes b;
            b.U8(0x81); // quads, format 1
            b.U16(4);
            for (int k = 0; k < 4; ++k) { b.U8(Rn(kVerts)); b.U32(Rnd() | 0xFFu); b.U8(Rn(kVerts)); }
            g_fxQuads.push_back(NewList(b));
        }
    }
    if (g_shStatic > 0 || g_shDyn > 0) {
        Material m = NewMaterial();
        g_mats.push_back(m);
        g_shMat = NewList(MaterialList(m));
        Bytes v; // (direct positions, XYZ f32, no other attribute: VTXFMT2)
        v.CP(0x50, 1u << 9);
        v.CP(0x60, 0);
        v.CP(0x72, 1u | (4u << 1));
        v.CP(0x82, 0);
        v.CP(0x92, 0);
        v.XF(0x1008, {0u});
        g_shFmt = NewList(v, 0xC0 - Pad(v.size()));
        g_shGeo = NewList(Geometry(0, 5)); // (the volume model's list: strips over the position / normal arrays, VTXFMT0)
        static uint32_t handles = 100000;
        g_shHandle = ++handles;
    }
    if (g_furLayers > 0) {
        // (the fur shape: a geometry list of its own, its material, and a position array per layer, each the model's pushed out a little)
        g_furGeo = NewList(Geometry(0, 24));
        Material m = NewMaterial();
        g_mats.push_back(m);
        g_furMat = NewList(MaterialList(m));
        for (int l = 0; l <= g_furLayers; ++l) {
            const uint32_t a = Alloc(kVerts * 12);
            for (uint32_t i = 0; i < kVerts * 3; ++i) {
                float v;
                std::memcpy(&v, g_ram + g_pos + i * 4, 4);
                PutLE(g_ram + a + i * 4, F2u(v * (1.0f + 0.02f * static_cast<float>(l))));
            }
            vgx::hle::NotifyGuestWrite(g_ram + a, kVerts * 12);
            g_furPos.push_back(a);
        }
    }
    if (g_plants > 0) {
        g_plantGeo = NewList(Geometry(0, 8));
        Material m = NewMaterial();
        g_mats.push_back(m);
        g_plantMat = NewList(MaterialList(m));
    }
    if (g_inst > 0) {
        g_instGeo = NewList(Geometry(0, 10));
        Material m = NewMaterial();
        g_mats.push_back(m);
        g_instMat = NewList(MaterialList(m));
        static uint32_t handles = 200000;
        g_instHandle = ++handles;
        for (int k = 0; k < g_inst; ++k) {
            const uint32_t a = Alloc(kVerts * 12);
            for (uint32_t i = 0; i < kVerts * 3; ++i) {
                float v;
                std::memcpy(&v, g_ram + g_pos + i * 4, 4);
                PutLE(g_ram + a + i * 4, F2u(v * (1.0f + 0.03f * static_cast<float>(k + 1))));
            }
            vgx::hle::NotifyGuestWrite(g_ram + a, kVerts * 12);
            g_instPos.push_back(a);
            uint32_t n = g_nrm;
            if (k & 1) {
                n = Alloc(kVerts * 12);
                for (uint32_t i = 0; i < kVerts * 3; ++i) {
                    float v;
                    std::memcpy(&v, g_ram + g_nrm + i * 4, 4);
                    PutLE(g_ram + n + i * 4, F2u(-v));
                }
                vgx::hle::NotifyGuestWrite(g_ram + n, kVerts * 12);
            }
            g_instNrm.push_back(n);
        }
    }
    if (g_jpaCount > 0) {
        Bytes dl;
        dl.U8(0x80); dl.U16(4);
        for (int k = 0; k < 4; ++k) { dl.U8(k); dl.U8(k); }
        g_jpaDl = NewList(dl);
        Bytes pos, crd;
        static const int8_t q[9][3] = {{0, 0, 0}, {50, 0, 0}, {50, -50, 0}, {0, -50, 0}, {-25, 0, 0}, {25, 0, 0}, {25, -50, 0}, {-25, -50, 0}, {-50, 0, 0}};
        for (int r = 0; r < 4; ++r)
            for (int i = 0; i < 9; ++i)
                for (int c = 0; c < 3; ++c) pos.U8(static_cast<uint8_t>(q[(i + r) % 9][c] + (c == 1 ? 25 * (r & 1) : 0)));
        g_jpaPos = NewList(pos);
        static const uint8_t t[16] = {0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 2, 0, 2, 1, 0, 1};
        for (uint8_t v : t) crd.U8(v);
        g_jpaCrd = NewList(crd);
        if (g_jpaVar) {
            Bytes x; // (jpa_dl_x: eight vertices, the second four from beyond the array's corners)
            x.U8(0x80); x.U16(8);
            for (int k = 0; k < 4; ++k) { x.U8(k); x.U8(k); }
            for (int k = 0; k < 4; ++k) { x.U8(0x48 + k); x.U8(k); }
            g_jpaDlX = NewList(x);
            Bytes b;
            b.BP(0xF3, 0x3F0000u);
            b.U8(0x80); b.U16(4);
            for (int k = 0; k < 4; ++k) { b.U8(k); b.U8(k); }
            g_jpaDlBp = NewList(b, 64 - Pad(b.size()) + 32);
            Bytes big;
            big.U8(0x80); big.U16(4);
            for (int k = 0; k < 4; ++k) { big.U8(k); big.U8(3 - k); }
            g_jpaDlBig = NewList(big, 96 - Pad(big.size()));
            Bytes two;
            for (int r = 0; r < 2; ++r) { two.U8(0x80); two.U16(4); for (int k = 0; k < 4; ++k) { two.U8((k + r) & 3); two.U8(k); } }
            g_jpaDl2 = NewList(two);
        }
        int left = g_jpaCount;
        while (left > 0) {
            JpaEmitter e;
            e.n = std::min(left, 1 + static_cast<int>(Rn(40)));
            left -= e.n;
            e.tex = static_cast<int>(Rn(kTex));
            e.zTest = Rn(3) != 0;
            e.env = Rn(2) == 0;
            e.texAnm = Rn(4) == 0;
            e.rot = Rn(3) == 0;
            e.flush = Rn(4) == 0;
            e.blendAdd = Rn(2) == 0;
            e.posOff = static_cast<int>(Rn(4)) * 27;
            e.crdOff = static_cast<int>(Rn(2)) * 8;
            e.prm = Rnd() | 0xFFu;
            e.envc = Rnd();
            e.var = g_jpaVar && Rn(2) == 0 ? 1 + static_cast<int>(Rn(8)) : 0;
            g_jpaEm.push_back(e);
        }
    }
}
// A particle loop's calls: as separate commands, or (FEW_FXMERGE=1) as the items of commands 0x58, written whenever the payload would
// pass `cap` bytes and at the loop's end (as GXDisplayList.c's gathering does).
struct JpaOut {
    Bytes& f;
    bool merged;
    uint32_t cap;
    Bytes items;
    JpaOut(Bytes& out, bool m, uint32_t c) : f(out), merged(m), cap(c) {}
    void Room(size_t n) { if (items.size() + n > cap) End(); }
    // (hotprof25 round 2, GXV_HOTPROF_TEVCOL, decomp/src/RVL_SDK/gx/GXTev.c: FEW_TEVCOL=1 writes a particle's TEV colour as the GX library
    // then does: the BG word once, and in a loop whose list writes no BP register a word equal to the loop's last for its register not at
    // all; =2 writes everything and announces each word 1 leaves out (markers BP 0xFF kinds 38 / 39: the front end checks it); =3 (negative
    // control) announces the first BG word of every colour as well, which is new when the colour changed. The command buffers must be
    // the same for 0, 1 and 2: tools/test_vgx_hotprof_tevcol.sh)
    bool scope = true;
    uint32_t lastRa[4] = {}, lastBg[4] = {};
    uint8_t lastValid[4] = {};
    void Mark(uint32_t reg, uint32_t value) {
        f.BP(0xFF, ((reg << 8 | ((value >> 16) & 0xFF)) << 8) | 38u);
        f.BP(0xFF, ((value & 0xFFFF) << 8) | 39u);
    }
    void Tev(uint32_t reg, uint32_t ra, uint32_t bg) {
        if (!merged) {
            static const int mode = std::getenv("FEW_TEVCOL") ? std::atoi(std::getenv("FEW_TEVCOL")) : 0;
            const uint32_t i = ((reg - 0xE0) >> 1) & 3;
            ra &= 0xFFFFFF;
            bg &= 0xFFFFFF;
            const bool eqRa = scope && (lastValid[i] & 1) && lastRa[i] == ra, eqBg = scope && (lastValid[i] & 2) && lastBg[i] == bg;
            if (mode == 0) {
                f.BP(reg, ra);
                for (int k = 0; k < 3; ++k) f.BP(reg + 1, bg);
            } else if (mode == 1) {
                if (!eqRa) f.BP(reg, ra);
                if (!eqBg) f.BP(reg + 1, bg);
            } else {
                if (eqRa) Mark(reg, ra);
                f.BP(reg, ra);
                if (eqBg || mode == 3) Mark(reg + 1, bg);
                f.BP(reg + 1, bg);
                for (int k = 0; k < 2; ++k) { Mark(reg + 1, bg); f.BP(reg + 1, bg); }
            }
            lastRa[i] = ra;
            lastBg[i] = bg;
            lastValid[i] = 3;
            return;
        }
        Room(9);
        items.U8(0x62);
        items.U32(reg << 24 | (ra & 0xFFFFFF));
        items.U32((reg + 1) << 24 | (bg & 0xFFFFFF));
    }
    void XF(uint32_t addr, const std::vector<uint32_t>& w) {
        if (!merged) { f.XF(addr, w); return; }
        Room(5 + w.size() * 4);
        items.U8(0x10); items.U16(static_cast<uint32_t>(w.size() - 1)); items.U16(addr);
        for (uint32_t x : w) items.U32(x);
    }
    void Call(uint32_t addr, uint32_t size, uint32_t flush) {
        // (hotprof25, GXV_HOTPROF_NOFLUSHSTRIP: FEW_NOFLUSH=1 leaves the flush primitives out as the GX library then does; the command
        // buffers must be the same: tools/test_vgx_hotprof_noflush.sh)
        static const bool noFlush = std::getenv("FEW_NOFLUSH") && std::atoi(std::getenv("FEW_NOFLUSH")) != 0;
        if (noFlush) flush = 0;
        if (!merged) {
            if (flush) { f.U8(0x98); f.U16(flush); for (uint32_t k = 0; k < flush * 2; ++k) f.U8(0); }
            f.Call(addr, size);
            return;
        }
        Room(10);
        items.U8(0x40); items.U32(addr); items.U32(size); items.U8(flush);
    }
    void End() {
        if (items.empty()) return;
        f.U8(0x58); f.U16(static_cast<uint32_t>(items.size()));
        f.insert(f.end(), items.begin(), items.end());
        items.clear();
    }
};
// One picture's particles (JPAEmitterManager::draw for one group).
void JpaFrame(Bytes& f, unsigned frame) {
    // (JPAEmitterManager::draw: TEV stage 0 ops, texture offsets, no culling, the vertex descriptor and formats, PNMTX0, no channels)
    f.BP(0xC0, (0xFu << 12) | (2u << 8) | (8u << 4) | 0xFu | (1u << 19));
    f.BP(0xC1, (7u << 13) | (4u << 10) | (1u << 7) | (7u << 4) | (1u << 19));
    f.CP(0x50, 2u << 9);   // POS index8
    f.CP(0x60, 2u);        // TEX0 index8
    f.CP(0x70, 1u | (3u << 1) | (1u << 21) | (3u << 22)); // fmt 0: XYZ s8, ST s8
    f.CP(0x80, 0);
    f.CP(0x90, 0);
    f.CP(0x71, 1u | (4u << 1) | (1u << 21) | (4u << 22)); // fmt 1: XYZ f32, ST f32
    f.XF(0x1008, {1u << 4});
    f.CP(0x30, 0 | (60u << 6));
    f.XF(0x1018, {0 | (60u << 6)});
    f.XF(0x1009, {0});
    for (const JpaEmitter& e : g_jpaEm) {
        // (JPABaseShape::setGX, JPAResource::setPTev, the texture load, the texgen)
        f.Masked(0x41, 0x00FFE7, e.blendAdd ? 0x0000A1u | (1u << 8) | (1u << 5) : 0x0034A1u);
        f.BP(0x40, e.zTest ? 0x07u : 0x00u);
        f.BP(0xF3, 0x3F0000u);
        f.BP(0xC0, (0xFu << 12) | ((e.env ? 4u : 2u) << 8) | (8u << 4) | (e.env ? 2u : 0xFu) | (1u << 19));
        f.BP(0xC1, (7u << 13) | (4u << 10) | (1u << 7) | (7u << 4) | (1u << 19));
        f.BP(0x10, 0);
        f.BP(0x11, 0);
        f.CP(0xA0, g_jpaPos.addr + static_cast<uint32_t>(e.posOff)); f.CP(0xB0, 3);
        f.CP(0xA4, g_jpaCrd.addr + static_cast<uint32_t>(e.crdOff)); f.CP(0xB4, 2);
        f.BP(0x28, 0x3C0u | (0x3C0u << 12));
        f.Masked(0x00, 0x07FC3F, 1u);
        const uint32_t ti = static_cast<uint32_t>(e.tex);
        f.BP(0x94, g_tex[ti] >> 5);
        f.BP(0x88, (g_texW[ti] - 1) | ((g_texH[ti] - 1) << 10) | (1u << 20));
        f.BP(0x80, 0x000195u);
        f.BP(0x84, 0);
        f.Masked(0x30, 0x03FFFF, g_texW[ti] - 1);
        f.Masked(0x31, 0x03FFFF, g_texH[ti] - 1);
        f.XF(0x1040, {0x5280});
        f.XF(0x1050, {0x3D});
        f.XF(0x103F, {1});
        f.CP(0x30, e.texAnm ? (30u << 6) : (60u << 6));
        f.XF(0x1018, {e.texAnm ? (30u << 6) : (60u << 6)});
        if (!e.texAnm) {
            // (JPARegistPrmEnv for the emitter)
            f.BP(0xE2, TevRA(static_cast<int>(e.prm >> 24), static_cast<int>(e.prm & 0xFF), false));
            for (int k = 0; k < 3; ++k) f.BP(0xE3, TevRA(static_cast<int>((e.prm >> 8) & 0xFF), static_cast<int>((e.prm >> 16) & 0xFF), false));
        }
        if (e.var == 5) { // (a projected texture: the texgen reads the position through TEXMTX0 3x4)
            f.XF(0x1040, {0x5000u | (1u << 1)});
            f.CP(0x30, 30u << 6);
            f.XF(0x1018, {30u << 6});
        }
        static const uint32_t capAll = std::getenv("FEW_FXCAP") ? static_cast<uint32_t>(std::atoi(std::getenv("FEW_FXCAP"))) : 8192u;
        JpaOut o(f, g_fxMerge, e.var == 8 ? 400u : capAll);
        o.scope = e.var != 2; // (FEW_TEVCOL: the list of this kind of emitter writes a BP register itself: no memory of the loop's colours)
        for (int i = 0; i < e.n; ++i) {
            // (the particle's colour: its alpha fades with its age)
            const int a = static_cast<int>((frame * 7 + static_cast<unsigned>(i) * 13) & 255);
            o.Tev(0xE2, TevRA(static_cast<int>(e.prm >> 24), a, false), TevRA(static_cast<int>((e.prm >> 8) & 0xFF), static_cast<int>((e.prm >> 16) & 0xFF), false));
            if (e.env)
                o.Tev(0xE4, TevRA(static_cast<int>(e.envc >> 24), static_cast<int>(e.envc & 0xFF), false),
                      TevRA(static_cast<int>((e.envc >> 8) & 0xFF), static_cast<int>((e.envc >> 16) & 0xFF), false));
            if (e.var == 6) o.Tev(0xE6, TevRA(static_cast<int>(a / 2), 255, false), TevRA(static_cast<int>(i & 255), 7, false));
            if (e.texAnm) {
                const float age = static_cast<float>(i) * 0.25f + static_cast<float>(frame & 63) * 0.1f;
                o.XF(0x78, {F2u(1.0f), 0, F2u(0.5f + age * 0.01f), 0, 0, F2u(1.0f), F2u(age * 0.02f), 0});
            }
            if (e.var == 4) o.XF(0x500, {F2u(1.0f), 0, 0, F2u(static_cast<float>(i)), 0, F2u(1.0f), 0, 0, 0, 0, F2u(1.0f), 0});
            std::vector<uint32_t> w(12, 0);
            const float sx = Rf(0.5f, 3), sy = Rf(0.5f, 3);
            if (e.rot) {
                const float c = Rf(-1, 1), sn = Rf(-1, 1);
                w[0] = F2u(c * sx); w[1] = F2u(-sn * sy); w[4] = F2u(sn * sx); w[5] = F2u(c * sy);
            } else {
                w[0] = F2u(sx); w[5] = F2u(sy);
            }
            w[10] = F2u(1.0f);
            w[3] = F2u(Rf(-30, 30)); w[7] = F2u(Rf(-20, 20)); w[11] = F2u(Rf(-200, -20));
            if (e.var == 5) o.XF(0x78, {w[0], w[1], w[2], F2u(0.5f), w[4], w[5], w[6], F2u(0.5f), 0, 0, F2u(1.0f), 0});
            o.XF(0, w);
            const List& l = e.var == 1 ? g_jpaDlX : e.var == 2 ? g_jpaDlBp : e.var == 3 ? g_jpaDlBig : e.var == 7 ? g_jpaDl2 : g_jpaDl;
            o.Call(l.addr, l.size, e.flush ? 3u : 0u);
        }
        o.End();
        if (e.var == 5) {
            f.XF(0x1040, {0x5280});
            f.CP(0x30, 60u << 6);
            f.XF(0x1018, {60u << 6});
        }
    }
}

// FEW_SCENE: the scene destroyed and made again: every list rewritten in place with other bytes, new holders (ids; with FEW_SCENE_REUSE=1 the
// same ids and versions, which a real game never does), the front end told (the state-list store dropped, GXV_SL_SCENEDROP=2).
void SceneChange() {
    for (Material& m : g_mats) { m.tevReg[2][1] ^= 0x40; m.konst[1][0] ^= 0x04; }
    for (Packet& k : g_pk) {
        Store(k.matList.addr, MaterialList(g_mats[k.mat]), k.matList.size);
        Store(k.diff[k.cur].addr, DiffList(g_mats[k.mat], k.diffKind), k.diff[k.cur].size);
        if (!g_sceneReuse) { k.matId = ++g_idCounter; k.diffId = ++g_idCounter; }
        k.matVer = k.diffVer = k.memberVer = 1;
        k.diffKinds = 0;
    }
    ++g_extScene;
    vgx::StateListsSceneGone(nullptr, nullptr, 0, 2);
}

// Between pictures: the differed lists made again into their other buffer (same bytes, or the animated values), objects come and go.
void Mutate(unsigned frame) {
    for (Material& m : g_mats) {
        if (!m.animated) continue;
        m.dReg[0][0] = TevRA(static_cast<int>((frame * 3) & 255), 255, false);
        m.dColor = (m.dColor & ~0xFF00u) | ((frame & 0xFF) << 8);
    }
    for (Packet& k : g_pk) {
        if (g_diffKeep) {
            // (GXV_J3D_DIFFKEEP: the list is made aside and stored, in the other buffer, only when its bytes differ from the current one's)
            const Bytes nb = DiffList(g_mats[k.mat], k.diffKind);
            const uint32_t room = k.diff[k.cur].size;
            std::vector<uint8_t> want(room, 0);
            std::memcpy(want.data(), nb.data(), nb.size());
            if (std::memcmp(g_ram + k.diff[k.cur].addr, want.data(), room) != 0) {
                k.cur ^= 1;
                Store(k.diff[k.cur].addr, nb, room);
                ++k.diffVer;
                k.diffKinds = 0x1u | 0x200u; // (a material colour, a texture matrix: J3DDiffFlag bits)
            }
        } else {
            k.cur ^= 1;
            Store(k.diff[k.cur].addr, DiffList(g_mats[k.mat], k.diffKind), k.diff[k.cur].size);
            ++k.diffVer;
        }
        for (Packet::Shape& sh : k.shapes) {
            if (sh.geoKind != 2) continue;
            // (FEW_BYPASS: a geometry list made again every picture, its size kept: the list cache bypasses it)
            Bytes g;
            const uint32_t room = sh.geo.size;
            while (g.size() + 80 < room) { // (a strip is 66 bytes at most)
                const uint32_t n = 4 + CsRn(6);
                g.U8(0x98); g.U16(n);
                for (uint32_t i = 0; i < n; ++i) { if (sh.kind == 1) g.U8(CsRn(4) * 3); g.U16(CsRn(kVerts)); g.U16(CsRn(kVerts)); g.U16(CsRn(kVerts)); }
            }
            Store(sh.geo.addr, g, room);
        }
        if (Rn(60) == 0) { k.visible = !k.visible; ++k.memberVer; }
    }
    if (Rn(3) == 0) {
        const size_t i = Rn(static_cast<uint32_t>(g_pk.size() - 1));
        std::swap(g_pk[i], g_pk[i + 1]);
    }
    for (List& o : g_odd) {
        if (Rn(20) == 0) {
            const Bytes b = OddList();
            if (Pad(b.size()) <= o.size) Store(o.addr, b, o.size);
        } else if (Rn(5) == 0) {
            std::vector<uint8_t> same(g_ram + o.addr, g_ram + o.addr + o.size);
            std::memcpy(g_ram + o.addr, same.data(), same.size());
            vgx::hle::NotifyGuestWrite(g_ram + o.addr, o.size); // (the same bytes written again)
        }
    }
    if (Rn(50) == 0) { // a material changed for good (and its list made again)
        Packet& k = g_pk[Rn(static_cast<uint32_t>(g_pk.size()))];
        g_mats[k.mat].tevReg[1][0] ^= 0x10;
        Store(k.matList.addr, MaterialList(g_mats[k.mat]), k.matList.size);
        if (!g_miss) ++k.matVer; // (FEW_J3N_MISS=1: a writer whose version bump is missing)
    }
    if (g_lightEvery > 0 && frame % static_cast<unsigned>(g_lightEvery) == 0) ++g_extLight;
    if (g_fogEvery > 0 && frame % static_cast<unsigned>(g_fogEvery) == 0) ++g_extFog;
    if (g_sceneEvery > 0 && frame % static_cast<unsigned>(g_sceneEvery) == 0) SceneChange();
}


// ---- FEW_TOP=<percent> (fe2-26): the console's top-level state writes between the J3D packets. The J3D packets above send their material state in display lists
// (the front end replays those as deltas); the game's other drawers (particles, layouts, fur, effects, the GX library's own set-up) write material state at top
// level, one command each. A picture at Honeyhive sends about 11,450 register writes this way, 71 % of which repeat what the register holds (fedl25, the console
// census of c77-hL; agents/agents/fedl25/REPORT2.txt section 2). The classes, their counts a picture and the share that repeat, are the census's; the registers used are
// ones the fixture's lists do not write, so a "repeat" is one. Percent scales the counts (100: the census's). From its own random sequence: the rest of the scene
// is the same with and without it.
uint64_t g_topRng = 0x2545F4914F6CDD1Dull;
uint32_t TopRn(uint32_t n) { g_topRng ^= g_topRng << 13; g_topRng ^= g_topRng >> 7; g_topRng ^= g_topRng << 17; return static_cast<uint32_t>(g_topRng >> 11) % n; }
int g_topPct = 0;
struct TopClass { const char* name; uint32_t perPic; uint32_t samePct; };
constexpr TopClass kTopClasses[] = {
    {"array-base/stride", 2030, 83}, {"tev-stage", 1777, 88}, {"tev-colour", 1551, 85}, {"tex", 1000, 65}, {"cp-mtxidx", 513, 89},
    {"xf-mtxidx", 325, 83}, {"chan", 350, 63}, {"texgen", 302, 88}, {"pixel", 607, 41}, {"idx-load", 1060, 0},
};
constexpr int kTopN = sizeof kTopClasses / sizeof kTopClasses[0];
uint32_t g_topVal[kTopN][64];
bool g_topHave[kTopN][64];
std::vector<Bytes> g_topScript; // (one packet's writes, made once: the game's drawers send the same sequence every picture)
size_t g_topScriptFor = 0;
void TopState(Bytes& out, size_t pi, size_t packets);
void TopStateMake(Bytes& f, size_t pi, size_t packets);
void TopState(Bytes& out, size_t pi, size_t packets) {
    if (g_topPct <= 0 || packets == 0) return;
    if (g_topScriptFor != packets) { g_topScript.clear(); g_topScriptFor = packets; }
    if (pi >= g_topScript.size()) {
        g_topScript.resize(pi + 1);
        TopStateMake(g_topScript[pi], pi, packets);
    }
    const Bytes& b = g_topScript[pi];
    out.insert(out.end(), b.begin(), b.end());
}
void TopStateMake(Bytes& f, size_t pi, size_t packets) {
    if (g_topPct <= 0 || packets == 0) return;
    static const int only = std::getenv("FEW_TOPONLY") ? std::atoi(std::getenv("FEW_TOPONLY")) : -1;   // (a single class, for per-class costs)
    static const bool rep = std::getenv("FEW_TOPREP") && std::atoi(std::getenv("FEW_TOPREP")) != 0;
    static const int sameForce = std::getenv("FEW_TOPSAME") ? std::atoi(std::getenv("FEW_TOPSAME")) : -1; // (0: every write changes, 100: every write repeats)
    for (int c = 0; c < kTopN; ++c) {
        if (only >= 0 && c != only) continue;
        const TopClass& k = kTopClasses[c];
        // (this packet's share: the counts spread over the packets, the remainder over the first ones)
        const uint64_t total = static_cast<uint64_t>(k.perPic) * static_cast<uint64_t>(g_topPct) / 100u;
        uint32_t n = static_cast<uint32_t>(total / packets) + ((pi % packets) < (total % packets) ? 1u : 0u);
        while (n--) {
            const uint32_t slot = TopRn(c == 0 ? 20 : c == 1 ? 16 : c == 2 ? 2 : c == 3 ? 12 : c == 4 ? 2 : c == 5 ? 1 : c == 6 ? 4 : c == 7 ? 8 : c == 8 ? 4 : 1);
            const bool same = g_topHave[c][slot] && TopRn(100) < (sameForce >= 0 ? static_cast<uint32_t>(sameForce) : k.samePct);
            uint32_t v = g_topVal[c][slot];
            if (!same) { // (another of the slot's four values: a drawer's states recur from picture to picture)
                uint32_t nv;
                do { nv = ((slot * 7919u + 1u + TopRn(4) * 104729u) * 2654435761u >> 8) | 1u; } while (g_topHave[c][slot] && nv == g_topVal[c][slot]);
                v = nv; g_topVal[c][slot] = v; g_topHave[c][slot] = true;
            }
            const size_t before = f.size();
            switch (c) {
            case 0: // CP array bases 0xA3, 0xA5..0xAF and strides 0xB3, 0xB5..0xBF (the fixture's lists and shapes use 0xA0, 0xA1, 0xA4)
                f.CP(slot < 10 ? (slot == 0 ? 0xA3u : 0xA4u + slot) : (slot == 10 ? 0xB3u : 0xB4u + (slot - 10)), v & 0x3FFFFF);
                break;
            case 1: f.BP(0xD0u + slot, v); break;                                  // TEV stage registers of stages 8..15
            case 2: if (slot == 0) { f.BP(0xE6u, v & 0x7FFFFF); f.BP(0xE7u, v & 0x7FFFFF); f.BP(0xE7u, v & 0x7FFFFF); f.BP(0xE7u, v & 0x7FFFFF); } else { f.BP(0xE4u, v & 0x7FFFFF); f.BP(0xE5u, v & 0x7FFFFF); f.BP(0xE5u, v & 0x7FFFFF); f.BP(0xE5u, v & 0x7FFFFF); } break;
            case 3: f.BP(0xB0u + slot, v); break;                                  // texture unit 4..7 registers (mode0 / mode1 / image0 / image3 of maps 4..7 are 0xA0..)
            case 4: f.CP(slot ? 0x40u : 0x30u, v & 0x3Fu); break;
            case 5: f.XF(0x1018, {v & 0x3Fu, v & 0xFC0u}); break;
            case 6: f.XF(0x1002u + slot, {v & 0x7Fu}); break;
            case 7: f.XF(0x1006u + (slot & 1u), {v & 0xFFFFFu}); break;
            case 8: f.BP(0x59u, v & 0x7u); break;
            case 9: f.U8(0x20); f.U16(TopRn(16)); f.U16((11u << 12) | 0u); break;  // an indexed matrix load: position matrix 0
            }
            if (rep) f.insert(f.end(), f.begin() + static_cast<std::ptrdiff_t>(before), f.begin() + static_cast<std::ptrdiff_t>(f.size())); // (FEW_TOPREP=1: the command sent twice: the second is a repeat for certain)
        }
    }
}

Bytes Frame() {
    Bytes f;
    CMark(f, 21, 0xFFFF); // (FEW_CENSUS: the picture is counted)
    f.CP(0xAC, g_mtxArr);
    f.CP(0xBC, 48);
    if (g_shStatic + g_shDyn > 0) { // (the game's EFB: RGBA6 + Z24, colour and alpha written, the scene's destination alpha the constant 0: GameScene::draw3D)
        f.Masked(0x43, 0x000007u, 1u);
        f.Masked(0x41, 0x000018u, 0x18u);
        f.BP(0x42, 0x000100u);
    }
    if (g_cam) { // (FEW_CAM=1: a moving camera)
        const float z = 0.05f * static_cast<float>(g_frame % 64);
        f.XF(0x1020, {F2u(1.2f + z), 0, F2u(1.6f + z), 0, F2u(-1.0f - 0.01f * z), F2u(-0.2f), 0});
    } else {
        f.XF(0x1020, {F2u(1.2f), 0, F2u(1.6f), 0, F2u(-1.0f), F2u(-0.2f), 0}); // (a perspective projection; the objects in front of the camera)
    }
    if (g_fogEvery > 0) { f.BP(0xEE, 0x000100u | (g_extFog & 0xFFu)); f.BP(0xEF, 0x400000u + g_extFog); } // (the scene's fog, at top level)
    int lastFmt = -1;
    size_t pi = 0;
    CMark(f, 21, 0x10F);
    static const bool j3dFmt = std::getenv("FEW_J3DFMT") && std::atoi(std::getenv("FEW_J3DFMT")) == 1;
    static const int light = std::getenv("FEW_LIGHTER") ? std::atoi(std::getenv("FEW_LIGHTER")) : 0;
    static const int dropPct = std::getenv("FEW_DROP") ? std::atoi(std::getenv("FEW_DROP")) : 0;
    int calledFmt = -1;                                  // (FEW_LIGHTER 1: the vertex-format list called last)
    size_t topIdx = 0, topVis = 0;                       // (FEW_TOP: the visible packets)
    for (const Packet& k : g_pk) topVis += k.visible ? 1u : 0u;
    uint32_t cpShadow[0x100] = {}; bool cpKnown[0x100] = {}; // (FEW_LIGHTER 4: top-level CP values as last written)
    Bytes dropped;                                       // (FEW_DROP: a dropped packet's bytes)
    Bytes& keep = f;
    for (const Packet& k : g_pk) {
        ++pi;
        if (!k.visible) continue;
        Bytes& f = (dropPct > 0 && (pi * 37u) % 100u < static_cast<size_t>(dropPct)) ? dropped : keep; // (FEW_DROP)
        if (j3dFmt) lastFmt = -1; // (J3DMatPacket::draw: J3DShape::resetVcdVatCache at every packet's end)
        auto cpW = [&](uint32_t reg, uint32_t v) {
            if ((light & 4) && cpKnown[reg] && cpShadow[reg] == v) return;
            if ((light & 8) && reg >= 0xA0 && reg <= 0xA2 && cpKnown[reg] && cpShadow[reg] == v) { // (GXV_ARRAYSKIP=2's markers: the front end checks)
                const uint32_t w = (light & 16) && pi % 17 == 0 ? v ^ 0x20u : v;                     // (16: a wrong one now and then: it must be seen)
                f.BP(0xFF, (24u + (reg - 0xA0u)) | ((w >> 16) << 8));
                f.BP(0xFF, 27u | ((w & 0xFFFFu) << 8));
            }
            f.CP(reg, v); cpShadow[reg] = v; cpKnown[reg] = true;
        };
        auto fmtCall = [&](int kind) {
            if (!((light & 1) && calledFmt == kind)) f.Call(g_fmt[kind].addr, g_fmt[kind].size);
            calledFmt = kind;
            if (j3dFmt) { cpShadow[0xA0] = g_pos; cpShadow[0xB0] = 12; cpShadow[0xA1] = g_nrm; cpShadow[0xB1] = 12; cpShadow[0xA4] = g_uv; cpShadow[0xB4] = 8;
                          for (uint32_t r : {0xA0u, 0xB0u, 0xA1u, 0xB1u, 0xA4u, 0xB4u}) cpKnown[r] = true; }
        };
        CMark(f, 22, static_cast<uint32_t>(pi % 4000) | (static_cast<uint32_t>(1 + pi % 8) << 12));
        if (g_topPct > 0) TopState(f, topIdx++, topVis);
        if (g_lightEvery > 0 && (pi % 3) == 0) { // (the actor lights of a light group, loaded at top level before the drawer: ActorLightCtrl::loadLight)
            std::vector<uint32_t> w;
            for (int i = 0; i < 16; ++i) w.push_back(g_extLight * 0x01000193u + static_cast<uint32_t>(i) * 977u + static_cast<uint32_t>(pi / 3 % 3) * 31u);
            f.XF(0x600u + 16u * static_cast<uint32_t>(pi / 3 % 3), w);
        }
        if (g_oddOn && Rn(40) == 0) f.BP(0xFE, 0x00F0FFu); // (the material list's first write behind the caller's mask)
        CallBracket(f, k.matList, k.matId, k.matVer, k.memberVer, 0);
        if (g_ddRand > 0) DdRand(f); // (FEW_GXDEDUP_RAND)
        if (!g_odd.empty() && Rn(6) == 0) {
            const List& o = g_odd[pi % g_odd.size()];
            f.Call(o.addr, o.size);
            calledFmt = -1; std::memset(cpKnown, 0, sizeof cpKnown); // (FEW_LIGHTER: an odd list may write anything)
            f.Call(g_fmt[0].addr, g_fmt[0].size); // (it may have written the vertex format: the shapes' own again)
            calledFmt = 0;
            lastFmt = 0;
            if (Rn(2)) { f.Call(o.addr, o.size); calledFmt = -1; std::memset(cpKnown, 0, sizeof cpKnown); } // (and once more, from the state it left)
        }
        bool firstShape = true;
        for (const Packet::Shape& sh : k.shapes) {
            if (!((light & 2) && !firstShape)) CallBracket(f, k.diff[k.cur], k.diffId, k.diffVer, k.memberVer, 1u | (k.diffKinds << 8));
            firstShape = false;
            if (sh.kind != lastFmt) { fmtCall(sh.kind); lastFmt = sh.kind; }
            if (!sh.idxMtx) {
                std::vector<uint32_t> w, n;
                for (int i = 0; i < 12; ++i) w.push_back(F2u((i % 5) == 0 ? Rf(0.5f, 2) : Rf(-0.5f, 0.5f)));
                w[3] = F2u(Rf(-5, 5)); w[7] = F2u(Rf(-4, 4)); w[11] = F2u(Rf(-40, -6));
                for (int i = 0; i < 9; ++i) n.push_back(F2u(Rf(-1, 1)));
                f.XF(0, w);
                f.XF(0x400, n);
            } else {
                f.U8(0x20); f.U16(Rn(16)); f.U16((11u << 12) | 0);
            }
            cpW(0x30, 0);
            cpW(0xA0, g_pos); cpW(0xB0, 12);
            cpW(0xA1, g_nrm); cpW(0xB1, 12);
            cpW(0xA4, g_uv); cpW(0xB4, 8);
            if (g_retained >= 1) {
                uint32_t h = sh.handle;
                if (g_retained == 2) {
                    const uint32_t k = RtRn(16);
                    if (k == 0) h = h % 7 + 1;      // (another shape's handle)
                    else if (k == 1) h = 0;         // (no handle)
                    else if (k == 2) { f.U8(0x59); f.U32(sh.handle); f.U8(0x44); } // (a handle with no call after it)
                }
                f.U8(0x59);
                f.U32(h);
            }
            f.Call(sh.geo.addr, sh.geo.size);
            if (g_ddRand > 0) DdRand(f); // (FEW_GXDEDUP_RAND)
        }
        CMark(f, 23, 0);
        if (g_copyEvery > 0 && pi % static_cast<size_t>(g_copyEvery) == 0) { // (FEW_COPY: an EFB copy to a texture, 64x64 RGBA8)
            f.BP(0x49, 0);
            f.BP(0x4A, 63u | (63u << 10));
            f.BP(0x4B, g_copyDst >> 5);
            f.BP(0x4D, 64u * 4u / 32u);
            f.BP(0x52, (6u << 4) | (1u << 3)); // (to a texture (bit 14 clear), format RGBA8, clamp)
            if (g_ddRand > 0) f.BP(0x52, (6u << 4) | (1u << 3)); // (FEW_GXDEDUP_RAND: the same trigger again: a register whose write is an action)
        }
    }
    CMark(f, 21, 0x7FFE);
    (void)dropped;
    if (g_plants > 0) {
        CMark(f, 21, 0x110);
        f.Call(g_plantMat.addr, g_plantMat.size);
        f.Call(g_fmt[0].addr, g_fmt[0].size);
        lastFmt = 0;
        for (int k = 0; k < g_plants; ++k) {
            std::vector<uint32_t> w, n;
            for (int i = 0; i < 12; ++i) w.push_back(F2u((i % 5) == 0 ? 1.0f : 0.0f));
            w[3] = F2u(Rf(-5, 5)); w[7] = F2u(Rf(-4, 4)); w[11] = F2u(Rf(-40, -10));
            for (int i = 0; i < 9; ++i) n.push_back(F2u((i % 4) == 0 ? 1.0f : 0.0f));
            f.XF(0, w);
            f.XF(0x400, n);
            f.CP(0x30, 0);
            f.CP(0xA0, g_pos); f.CP(0xB0, 12);
            f.CP(0xA1, g_nrm); f.CP(0xB1, 12);
            f.CP(0xA4, g_uv); f.CP(0xB4, 8);
            f.Call(g_plantGeo.addr, g_plantGeo.size);
        }
    }
    if (g_inst > 0) {
        CMark(f, 21, 0x113);
        static size_t instPic = 0; // (pi does not move from one picture to the next in this builder)
        ++instPic;
        if (instPic % 5 == 0) { // (an instance's model animates: its position array is written again with other values; the list is the same)
            const size_t k = (instPic / 5) % static_cast<size_t>(g_inst);
            for (uint32_t i = 0; i < kVerts * 3; ++i) {
                float v;
                std::memcpy(&v, g_ram + g_instPos[k] + i * 4, 4);
                PutLE(g_ram + g_instPos[k] + i * 4, F2u(v * (i % 7 == 0 ? 1.01f : 1.0f)));
            }
            vgx::hle::NotifyGuestWrite(g_ram + g_instPos[k], kVerts * 12);
        }
        if (instPic % 11 == 0) { // (a write that stores what was there)
            const size_t k = (instPic / 11) % static_cast<size_t>(g_inst);
            vgx::hle::NotifyGuestWrite(g_ram + g_instPos[k], kVerts * 12);
        }
        f.Call(g_instMat.addr, g_instMat.size);
        f.Call(g_fmt[0].addr, g_fmt[0].size);
        lastFmt = 0;
        for (int k = 0; k < g_inst; ++k) {
            std::vector<uint32_t> w, n;
            for (int i = 0; i < 12; ++i) w.push_back(F2u((i % 5) == 0 ? 1.0f : 0.0f));
            w[3] = F2u(InstRf(-5, 5)); w[7] = F2u(InstRf(-4, 4)); w[11] = F2u(InstRf(-40, -10));
            for (int i = 0; i < 9; ++i) n.push_back(F2u((i % 4) == 0 ? 1.0f : 0.0f));
            f.XF(0, w);
            f.XF(0x400, n);
            f.CP(0x30, 0);
            f.CP(0xA0, g_instPos[static_cast<size_t>(k)]); f.CP(0xB0, 12);
            f.CP(0xA1, g_instNrm[static_cast<size_t>(k)]); f.CP(0xB1, 12);
            f.CP(0xA4, g_uv); f.CP(0xB4, 8);
            if (g_retained >= 1) { f.U8(0x59); f.U32(g_instHandle); }
            f.Call(g_instGeo.addr, g_instGeo.size);
        }
    }
    CMark(f, 21, 0x111);
    for (int k = 0; k < (g_furLayers > 0 ? g_furCount : 0); ++k) {
        // (FEW_FUR: the model's own draw, then FurCtrl::drawFur's layers: setupLayerMaterial's TEV colour, prepareDraw's matrices, drawFast's
        // vertex format and arrays (the layer's positions), the shape's list)
        f.Call(g_furMat.addr, g_furMat.size);
        std::vector<uint32_t> w, n;
        for (int i = 0; i < 12; ++i) w.push_back(F2u((i % 5) == 0 ? Rf(0.5f, 2) : Rf(-0.5f, 0.5f)));
        w[3] = F2u(Rf(-5, 5)); w[7] = F2u(Rf(-4, 4)); w[11] = F2u(Rf(-40, -6));
        for (int i = 0; i < 9; ++i) n.push_back(F2u(Rf(-1, 1)));
        for (int l = 0; l <= g_furLayers; ++l) {
            if (l > 0) {
                f.BP(0xE2, TevRA(16 * l & 255, 255 - 8 * l, false));
                f.BP(0xE3, TevRA(255, 255, false));
            }
            f.Call(g_fmt[0].addr, g_fmt[0].size);
            lastFmt = 0;
            f.XF(0, w);
            f.XF(0x400, n);
            f.CP(0x30, 0);
            f.CP(0xA0, g_furPos[static_cast<size_t>(l)]); f.CP(0xB0, 12);
            f.CP(0xA1, g_nrm); f.CP(0xB1, 12);
            f.CP(0xA4, g_uv); f.CP(0xB4, 8);
            f.Call(g_furGeo.addr, g_furGeo.size);
        }
    }
    CMark(f, 21, 0x400);
    for (int c = 0; c < g_shStatic + g_shDyn; ++c) {
        // (ShadowVolumeDrawer::draw: loadModelDrawMtx, then twice drawShape under other cull / blend modes; a volume is a few units across, in front)
        const bool dyn = c >= g_shStatic;
        if (c == 0) f.Call(g_shMat.addr, g_shMat.size);
        f.Call(dyn ? g_shFmt.addr : g_fmt[0].addr, dyn ? g_shFmt.size : g_fmt[0].size);
        std::vector<uint32_t> w;
        const float sc = ShRf(0.4f, 1.6f);
        for (int i = 0; i < 12; ++i) w.push_back(F2u((i % 5) == 0 ? sc * ShRf(0.5f, 1.5f) : sc * ShRf(-0.4f, 0.4f)));
        w[3] = F2u(ShRf(-6, 6)); w[7] = F2u(ShRf(-4, 4)); w[11] = F2u(ShRf(-45, -7));
        f.XF(0, w);
        f.CP(0x30, 0);
        f.BP(0x42, 0x000000u);                                   // (GXSetDstAlpha(FALSE, 0))
        f.BP(0x40, 0x00000Du);                                   // (depth GEQUAL, no update)
        float pts[14][3]; // (ShadowVolumeBox's mPoints: 14 points, which its three primitives name 30 times)
        for (int i = 0; i < 14; ++i) for (int k = 0; k < 3; ++k) pts[i][k] = ShRf(-1, 1) * (k == 1 ? 3.0f : 1.0f);
        if (!dyn) { f.CP(0xA0, g_pos); f.CP(0xB0, 12); f.CP(0xA1, g_nrm); f.CP(0xB1, 12); f.CP(0xA4, g_uv); f.CP(0xB4, 8); }
        for (int pass = 0; pass < 2; ++pass) {
            f.Masked(0x00, 0x00C000u, (pass == 0 ? 2u : 1u) << 14);                                      // (GXSetCullMode: front, then back)
            f.BP(0x41, pass == 0 ? (1u | (1u << 5) | (1u << 8) | (1u << 4)) : ((1u << 11) | (1u << 4))); // (blend ONE ONE; subtract; alpha update only)
            if (!dyn) {
                f.U8(0x59);
                f.U32(g_shHandle);
                f.Call(g_shGeo.addr, g_shGeo.size);
            } else {
                static const uint32_t cnt[3] = {14, 8, 8};
                static const uint8_t op[3] = {0x9A, 0xA2, 0xA2};
                static const uint8_t ix[3][14] = {{0, 1, 2, 3, 10, 11, 6, 7, 4, 5, 8, 9, 0, 1}, {12, 0, 2, 10, 6, 4, 8, 0}, {13, 1, 9, 5, 7, 11, 3, 1}}; // (drawShape)
                for (int s3 = 0; s3 < 3; ++s3) {
                    f.U8(op[s3]);
                    f.U16(cnt[s3]);
                    for (uint32_t i = 0; i < cnt[s3]; ++i)
                        for (int k = 0; k < 3; ++k) f.U32(F2u(pts[ix[s3][i]][k]));
                }
            }
        }
        f.BP(0x41, 1u << 3 | 1u << 4);                           // (GXSetColorUpdate(1) again)
        f.BP(0x42, 0x000100u);                                   // (GXSetDstAlpha(TRUE, 0))
    }
    CMark(f, 21, 0x012);
    for (int i = 0; i < g_fxCount; ++i) {
        if (i == 0 || i == g_fxCount / 2) {
            f.Call(g_fxMat[i == 0 ? 0 : 1].addr, g_fxMat[i == 0 ? 0 : 1].size);
            f.Call(g_fxFmt.addr, g_fxFmt.size);
            f.CP(0x30, 0);
            f.CP(0xA0, g_pos); f.CP(0xB0, 12);
            f.CP(0xA4, g_uv); f.CP(0xB4, 8);
        }
        std::vector<uint32_t> w;
        for (int k = 0; k < 12; ++k) w.push_back(F2u((k % 5) == 0 ? 1.0f : 0.0f));
        w[3] = F2u(Rf(-3, 3)); w[7] = F2u(Rf(-2, 2)); w[11] = F2u(Rf(-20, -8));
        f.XF(0, w);
        f.BP(0xE2, TevRA(static_cast<int>(Rn(256)), 255, false));
        f.BP(0xE3, TevRA(static_cast<int>(Rn(256)), static_cast<int>(Rn(256)), false));
        const List& q = g_fxQuads[static_cast<size_t>(i) % g_fxQuads.size()];
        f.Call(q.addr, q.size);
    }
    CMark(f, 21, 0x030);
    if (g_jpaCount > 0) JpaFrame(f, g_frame);
    CMark(f, 21, 0x031);
    if (g_bigCount > 0) BigFrame(f);
    if (g_mixedN > 0) { // (FEW_MIXED: mixed lists, after the material of the first packet)
        if (!g_pk.empty()) f.Call(g_pk[0].matList.addr, g_pk[0].matList.size);
        const int fmt = g_frame % 7 == 3 ? 1 : 0; // (now and then the other vertex format: a draw of another size)
        f.Call(g_fmt[fmt].addr, g_fmt[fmt].size);
        f.CP(0xA0, g_pos); f.CP(0xB0, 12);
        f.CP(0xA1, g_nrm); f.CP(0xB1, 12);
        f.CP(0xA4, g_uv); f.CP(0xB4, 8);
        for (int i = 0; i < g_mixedN; ++i) {
            const Mixed& m = g_mixed[static_cast<size_t>(i) % g_mixed.size()];
            f.Call(m.l.addr, m.l.size);
        }
    }
    CMark(f, 21, 0x7FFE);
    return f;
}

bool g_quiet = false;
int64_t g_feCpuUs = 0, g_feCpuLate = 0, g_feLateN = 0; // (FEW_REALTIME=1: the player's CPU in the front end: the stream's parts and the picture's end)
uint64_t g_h = 0;
void Mix(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) g_h = (g_h ^ b[i]) * 1099511628211ull;
}
bool hashAbOn() { static const bool on = std::getenv("FEW_HASHAB") != nullptr; return on; }
uint64_t g_abRecords = 0, g_awRecords = 0; // (twocore24: records with alpha bounds, records that write alpha, for the log)
void Observe(vgx::CmdBuf& b) {
    if (!g_quiet) {
        g_h = 14695981039346656037ull;
        for (const vgx::Cmd& c : b.cmds) { const uint32_t v[2] = {c.type, c.arg}; Mix(v, sizeof v); }
        for (const vgx::DrawRec& d : b.draws) {
            const uint32_t v[16] = {d.vsId, d.fsId, d.prim, d.texMask, d.cover, static_cast<uint32_t>(d.exX), static_cast<uint32_t>(d.exY),
                                    static_cast<uint32_t>(d.exW), static_cast<uint32_t>(d.exH), d.layoutMask, d.vbuf, d.vtxOffset, d.vtxCount, d.idxOffset,
                                    d.idxCount, d.vtxFirst};
            Mix(v, sizeof v);
            Mix(d.ublock, sizeof d.ublock);
            Mix(&d.rs, sizeof d.rs);
            Mix(d.tex, sizeof d.tex);
            // (shadowfe24 + twocore24: GXV_SHADOWFILL_BOUNDS / GXV_SHADOW_RECT's alpha bounds and the culled cover bit, which the picture's
            // end reads, go into the hash when FEW_HASHAB is set: tools/test_vgx_twocore.sh sets it; the shadow-rect test compares
            // against binaries from before the field existed and sets it only where both sides have it)
            static const bool hashAb = std::getenv("FEW_HASHAB") != nullptr;
            if (hashAb) {
                const int32_t ab[6] = {d.ab, d.abX, d.abY, d.abW, d.abH, d.coverCull};
                Mix(ab, sizeof ab);
            }
            if (d.ab) ++g_abRecords;
            if (d.rs.alphaWrite) ++g_awRecords;
        }
        Mix(b.vtx.data(), b.vtx.size());
        Mix(b.idx.data(), b.idx.size() * 2);
        Mix(b.uni.data(), b.uni.size() * 4);
        for (const vgx::BufUpload& u : b.bufUploads) { Mix(u.vtx.data(), u.vtx.size()); Mix(u.idx.data(), u.idx.size() * 2); }
        for (const vgx::TexUpload& u : b.uploads) for (const auto& l : u.levels) Mix(l.data(), l.size());
        static const long dump = std::getenv("FEW_DUMP") ? std::atol(std::getenv("FEW_DUMP")) : -1; // (a frame's commands and records, on stderr)
        if (dump == static_cast<long>(g_frame)) {
            for (size_t i = 0; i < b.cmds.size(); ++i) {
                const vgx::Cmd& c = b.cmds[i];
                std::fprintf(stderr, "dump cmd %zu: type %u arg %u", i, c.type, c.arg);
                if (c.type == vgx::CMD_DRAW && c.arg < b.draws.size()) {
                    const vgx::DrawRec& d = b.draws[c.arg];
                    std::fprintf(stderr, " vs %u fs %u prim %u tm %u lm %x vbuf %u vo %u vc %u io %u ic %u vf %u ub", d.vsId, d.fsId, d.prim, d.texMask, d.layoutMask,
                                 d.vbuf, d.vtxOffset, d.vtxCount, d.idxOffset, d.idxCount, d.vtxFirst);
                    for (uint32_t u : d.ublock) std::fprintf(stderr, " %u", u);
                    uint64_t h = 1469598103934665603ull;
                    const uint8_t* r = reinterpret_cast<const uint8_t*>(&d.rs);
                    for (size_t k = 0; k < sizeof d.rs; ++k) h = (h ^ r[k]) * 1099511628211ull;
                    const uint8_t* t = reinterpret_cast<const uint8_t*>(d.tex);
                    for (size_t k = 0; k < sizeof d.tex; ++k) h = (h ^ t[k]) * 1099511628211ull;
                    std::fprintf(stderr, " rs/tex %016llx", static_cast<unsigned long long>(h));
                }
                std::fprintf(stderr, "\n");
            }
            for (size_t i = 0; i < b.uni.size(); ++i) {
                uint32_t w;
                std::memcpy(&w, &b.uni[i], 4);
                std::fprintf(stderr, "dump uni %zu: %08x\n", i, w);
            }
        }
        if (hashAbOn()) {
            size_t nab = 0, nw = 0, nbl = 0;
            for (const vgx::DrawRec& d : b.draws) { nab += d.ab != 0; nw += d.rs.alphaWrite != 0; nbl += d.rs.blend != 0; }
            std::fprintf(stderr, "ab frame %u: %zu records with alpha bounds, %zu write alpha, %zu blended\n", g_frame, nab, nw, nbl);
        }
        std::printf("frame %u: %016llx  %zu cmds %zu draws %zu vtx %zu idx %zu uni %zu uploads\n", g_frame, static_cast<unsigned long long>(g_h), b.cmds.size(),
                    b.draws.size(), b.vtx.size(), b.idx.size(), b.uni.size(), b.bufUploads.size());
    }
    vgx::backend::Execute(b);
}
} // namespace

namespace vgx { void SlDeltaCounters(uint64_t out[5]) __attribute__((weak)); }
namespace vgx { void FePlanCounters(uint64_t out[8]) __attribute__((weak)); } // (GXV_FEPLAN; weak, as above) // (weak: the fixture also builds against sources without it)
namespace vgx { void TinyListCounters(uint64_t out[8]) __attribute__((weak)); } // (tinylist31, GXV_TINYLIST; weak, as above)
namespace vgx { namespace split { void FxJoinCounters(uint64_t out[2]) __attribute__((weak)); } } // (GXV_FXJOIN; weak, as above)
namespace vgx { void FxMergeCounters(uint64_t out[6]) __attribute__((weak)); } // (GXV_FXMERGE; weak, as above)
namespace vgx { void RetainedCounters(uint64_t out[7]) __attribute__((weak)); } // (GXV_RETAINED; weak, as above)
namespace vgx { void DlVariantCounters(uint64_t out[3]) __attribute__((weak)); } // (GXV_DL_VARIANTS; weak, as above)
namespace vgx { void DlCacheBranchCounts(uint64_t out[7]); }
namespace vgx { namespace fepar { void Counters(uint64_t out[8]) __attribute__((weak)); } } // (GXV_FE_PARALLEL; weak, as above)
namespace vgx { void ShadowRectFeCounters(uint64_t out[7]) __attribute__((weak)); int ShadowRectFeMode() __attribute__((weak)); } // (GXV_SHADOW_RECT_FE; weak, as above)
namespace vgx { void DirectJ3DCounters(uint64_t out[12]) __attribute__((weak)); } // (GXV_DIRECT_J3D; weak, as above)
namespace vgx { void J3nCounters(uint64_t out[24]) __attribute__((weak)); } // (GXV_J3DNATIVE; weak, as above)
namespace vgx { void J3n2Counters(uint64_t out[24]) __attribute__((weak)); } // (GXV_J3DNATIVE=4 / 5; weak, as above)
namespace vgx { namespace two { void PublishedUpTo(const uint8_t* end) __attribute__((weak)); void ReportNow() __attribute__((weak)); } } // (GXV_FE_TWOCORE; weak, as above)
namespace vgx { namespace two { void SetThreadInit(int (*fn)(int core)) __attribute__((weak)); } }

// ---- twocore24: FEW_GAME=1, the console's threads around the front end (GXV_FE_TWOCORE's freeze on the console, c42 job 854)
// The plain loop above hands the player a whole picture at once. Here, as on the console:
//   - a game thread makes each picture and publishes it in pieces (2-6 KiB, random pauses) while the player (this thread) parses them;
//     before it publishes, it hands a "view job" to a worker (the par-view worker: GXV_PAR_VIEW=2) and, after its first piece, waits for
//     that job WITHOUT a bound and without sleeping, as the game's draw does (par_async.h Ensure); after the picture it waits for the
//     player's finish (the draw-done) and only then changes guest memory (Mutate);
//   - a loading thread waits for each of the player's finishes on its own (GXV_DRAWDONE_OTHER's waits);
//   - FEW_SCENE_DROP=<n>: every n pictures a scene goes: the player drops the EFB-copy textures, state lists and textures of the whole guest
//     RAM (GXV_SL_SCENEDROP / GXV_TEX_SCENEDROP's modes, as vgx_galaxy.cpp CopyDropService) before the next picture's first bytes;
//   - FEW_SEGMOVE=<n>: every n pictures the game moves to another stream segment in the middle of the picture (GXV_SEG_IDLE: the player
//     idle, its unparsed bytes copied to the new segment's start), so the player is handed other memory mid-picture;
//   - FEW_GAME_RT=1 (needs SCHED_FIFO): the worker and the second worker (B) on CPU 1 with real-time priorities in the console's order
//     (0xC0 minus the console's priority: the worker 0x9E, B GXV_FE_TWOCORE_PRIO, default 0xA0, raised to 0x8E by port_vgx2_boost), the
//     rest on CPU 0: a thread spinning at B's priority keeps the worker off CPU 1 as the console's scheduler does;
//   - FEW_GAME_WATCH=<s>: no progress for s seconds (default 20) = "fixture: HANG", exit code 3.
namespace fewgame {
constexpr size_t kSegBytes = 4u << 20;
uint8_t* g_seg[3] = {};
std::atomic<uint64_t> g_pub{0};      // segment << 32 | bytes published in it (the game's)
std::atomic<uint32_t> g_pubFrame{0}; // the frame whose bytes are being published
std::atomic<uint32_t> g_complete{0}; // the frame whose bytes are all published (g_endState: where they end)
std::atomic<uint64_t> g_endState{0};
std::atomic<uint64_t> g_plSeen{0};   // (the player's) the publication it has parsed as far as it could, and its read position there
std::atomic<uint32_t> g_plRd{0};
std::atomic<uint32_t> g_finished{0}; // the frame the player has finished (its display copy made)
std::atomic<int64_t> g_progress{0};
std::mutex g_mtx;
std::condition_variable g_cvPlayer, g_cvFinish;
std::atomic<int> g_job{2};           // the view job: 0 handed out, 1 the worker is in it, 2 done, 3 taken by the game
std::atomic<uint32_t> g_jobSeq{0};
std::atomic<bool> g_stop{false};
std::mutex g_wMtx;
std::condition_variable g_wCv;
uint32_t g_jobUs = 3000;
std::atomic<uint64_t> g_loadWaits{0}, g_jobWaitsSpun{0}, g_segMoves{0};
pthread_t g_bThread;
bool g_bThreadSet = false, g_rt = false;
int g_bPrio = 32;
int64_t Now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void Busy(uint32_t us) {
    const int64_t end = Now() + static_cast<int64_t>(us) * 1000;
    volatile uint64_t x = 0;
    while (Now() < end) for (int i = 0; i < 64; ++i) x = x + static_cast<uint64_t>(i);
}
void Place(int cpu, int fifo) {
    if (!g_rt) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    if (fifo > 0) {
        sched_param sp{};
        sp.sched_priority = fifo;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) std::fprintf(stderr, "fixture: SCHED_FIFO %d refused\n", fifo);
    }
}
int Fifo(long vitaPrio) { return static_cast<int>(0xC0 - vitaPrio); } // (the console: a lower number runs first; here a higher one)
// (GXV_FE_TWOCORE's thread placement hook, vgx_twocore_impl.h SetThreadInit)
int TwoInit(int) {
    g_bThread = pthread_self();
    g_bThreadSet = true;
    const char* e = std::getenv("GXV_FE_TWOCORE_PRIO");
    g_bPrio = Fifo(e ? std::strtol(e, nullptr, 0) : 0xA0);
    Place(1, g_bPrio);
    return 1;
}
void WorkerMain() {
    Place(1, Fifo(0x9E));
    uint32_t seen = 0;
    while (!g_stop.load()) {
        {
            std::unique_lock<std::mutex> lk(g_wMtx);
            g_wCv.wait_for(lk, std::chrono::milliseconds(1), [&] { return g_jobSeq.load() != seen || g_stop.load(); });
        }
        if (g_jobSeq.load() == seen) continue;
        seen = g_jobSeq.load();
        int v = 0;
        if (g_job.compare_exchange_strong(v, 1)) {
            Busy(g_jobUs);
            g_job.store(2, std::memory_order_release);
        }
    }
}
// (the draw touches the job: takes it when nobody has, else waits for the worker, spinning: par_async.h Ensure)
void Ensure() {
    int v = 0;
    if (g_job.compare_exchange_strong(v, 3)) {
        Busy(g_jobUs);
        g_job.store(2, std::memory_order_release);
        return;
    }
    if (g_job.load(std::memory_order_acquire) != 2) g_jobWaitsSpun.fetch_add(1);
    while (g_job.load(std::memory_order_acquire) != 2) {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("pause" ::: "memory");
#endif
    }
}
void LoadingMain() {
    uint32_t seen = 0;
    while (!g_stop.load()) {
        std::unique_lock<std::mutex> lk(g_mtx);
        g_cvFinish.wait_for(lk, std::chrono::milliseconds(5), [&] { return g_finished.load() != seen || g_stop.load(); });
        if (g_finished.load() != seen) { seen = g_finished.load(); g_loadWaits.fetch_add(1); }
    }
}
void Notify(std::condition_variable& cv) {
    std::lock_guard<std::mutex> lk(g_mtx);
    cv.notify_all();
}
void GameMain(unsigned frames, size_t trashBytes, uint64_t* bytes) {
    Place(0, 0);
    uint32_t rng = 0x12345678u;
    auto rn = [&rng](uint32_t n) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng % n; };
    static const int segMove = std::getenv("FEW_SEGMOVE") ? std::atoi(std::getenv("FEW_SEGMOVE")) : 0;
    uint32_t seg = 0;
    for (unsigned f = 1; f <= frames && !g_stop.load(); ++f) {
        g_frame = f; // (the player reads it after the publication below)
        Bytes fr = Frame();
        *bytes += fr.size();
        if (fr.size() > kSegBytes) { std::fprintf(stderr, "fixture: a picture of %zu bytes is too large\n", fr.size()); std::_Exit(2); }
        seg = (seg + 1) % 3;
        std::memcpy(g_seg[seg], fr.data(), fr.size());
        // (the drawn turn: its view jobs go to the worker, then the draw publishes and touches them)
        g_job.store(0, std::memory_order_release);
        g_jobSeq.fetch_add(1);
        { std::lock_guard<std::mutex> lk(g_wMtx); g_wCv.notify_all(); }
        std::this_thread::sleep_for(std::chrono::microseconds(200)); // (the worker gets into the job)
        g_pubFrame.store(f, std::memory_order_release);
        size_t pub = 0, base = 0; // (base: where this segment's bytes start in the picture)
        bool touched = false, moved = false;
        while (pub < fr.size()) {
            pub = std::min(fr.size(), pub + 2048 + rn(4096));
            if (segMove > 0 && !moved && f % static_cast<unsigned>(segMove) == 0 && pub > fr.size() / 2 && pub < fr.size()) {
                // (GXV_SEG_IDLE: the player has parsed everything published; what it left (a cut command) and the rest go to another segment)
                moved = true;
                const uint64_t st = g_pub.load();
                while (g_plSeen.load(std::memory_order_acquire) != st && !g_stop.load()) std::this_thread::sleep_for(std::chrono::microseconds(50));
                const size_t rd = base + g_plRd.load(std::memory_order_acquire);
                const uint32_t next = (seg + 1) % 3;
                std::memcpy(g_seg[next], fr.data() + rd, fr.size() - rd);
                seg = next;
                base = rd;
                g_segMoves.fetch_add(1);
            }
            g_pub.store((static_cast<uint64_t>(seg) << 32) | static_cast<uint32_t>(pub - base), std::memory_order_release);
            Notify(g_cvPlayer);
            if (!touched) { touched = true; Ensure(); }
            if (rn(3) == 0) std::this_thread::sleep_for(std::chrono::microseconds(rn(300)));
        }
        g_endState.store((static_cast<uint64_t>(seg) << 32) | static_cast<uint32_t>(fr.size() - base), std::memory_order_release);
        g_complete.store(f, std::memory_order_release);
        Notify(g_cvPlayer);
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            while (g_finished.load() < f && !g_stop.load()) g_cvFinish.wait_for(lk, std::chrono::milliseconds(5));
        }
        Mutate(f);
        if (trashBytes) {
            static std::vector<uint8_t> trash(trashBytes);
            for (size_t i = 0; i < trash.size(); i += 32) trash[i] = static_cast<uint8_t>(trash[i] + f);
        }
    }
}
// The player: this thread.
int Run(unsigned frames, size_t trashBytes, uint64_t* bytes) {
    g_rt = std::getenv("FEW_GAME_RT") && std::atoi(std::getenv("FEW_GAME_RT")) != 0;
    if (std::getenv("FEW_GAME_JOB_US")) g_jobUs = static_cast<uint32_t>(std::atoi(std::getenv("FEW_GAME_JOB_US")));
    const int watch = std::getenv("FEW_GAME_WATCH") ? std::atoi(std::getenv("FEW_GAME_WATCH")) : 20;
    const int sceneDrop = std::getenv("FEW_SCENE_DROP") ? std::atoi(std::getenv("FEW_SCENE_DROP")) : 0;
    const int slMode = std::getenv("GXV_SL_SCENEDROP") ? std::atoi(std::getenv("GXV_SL_SCENEDROP")) : 0;
    // (never freed: a second worker left behind may still read them. FEW_SEG_GAP=1: 32 MiB apart, so that no picture starts within 16 MiB below
    // the end of the one before: the trees before twocore24 then do not show their stale-end fault, only the priority one)
    if (std::getenv("FEW_SEG_GAP") && std::atoi(std::getenv("FEW_SEG_GAP")) != 0) {
        uint8_t* const all = static_cast<uint8_t*>(std::calloc(3 * (32u << 20), 1));
        for (int i = 0; i < 3; ++i) g_seg[i] = all + i * (32u << 20);
    } else {
        for (uint8_t*& p : g_seg) p = static_cast<uint8_t*>(std::calloc(kSegBytes, 1));
    }
    if (&vgx::two::SetThreadInit) vgx::two::SetThreadInit(&TwoInit);
    Place(0, 0);
    std::thread worker(WorkerMain), loading(LoadingMain);
    std::thread game(GameMain, frames, trashBytes, bytes);
    // (the watch: no finished picture for `watch` seconds)
    std::thread watcher([watch] {
        int64_t last = g_progress.load();
        uint32_t lastF = 0;
        int64_t since = Now();
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const uint32_t fNow = g_finished.load();
            (void)last;
            if (fNow != lastF) { lastF = fNow; since = Now(); continue; }
            if (Now() - since > static_cast<int64_t>(watch) * 1000000000ll) {
                std::fprintf(stderr, "fixture: HANG: no progress for %d s; pictures finished %u, published frame %u (complete %u), the view job's state %d, "
                                     "the game thread waited spinning for it %llu times\n", watch, fNow, g_pubFrame.load(), g_complete.load(),
                             g_job.load(), static_cast<unsigned long long>(g_jobWaitsSpun.load()));
                std::fflush(stdout);
                std::fflush(stderr);
                std::_Exit(3);
            }
        }
    });
    for (unsigned f = 1; f <= frames; ++f) {
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            while (g_pubFrame.load(std::memory_order_acquire) < f) g_cvPlayer.wait_for(lk, std::chrono::milliseconds(1));
        }
        if (sceneDrop > 0 && f > 1 && (f - 1) % static_cast<unsigned>(sceneDrop) == 0) {
            // (vgx_galaxy.cpp CopyDropService: before the first bytes published after the scene's destruction, on the front end's thread)
            const void* lo[1] = {g_ram};
            const void* hi[1] = {g_ram + kRam};
            vgx::CopyTexturesInRange(lo[0], hi[0], true);
            vgx::StateListsSceneGone(lo, hi, 1, slMode);
            vgx::TextureCacheSceneGone(lo, hi, 1, vgx::TexSceneDropMode());
        }
        uint64_t st = g_pub.load(std::memory_order_acquire);
        uint32_t seg = static_cast<uint32_t>(st >> 32);
        size_t rd = 0;
        for (;;) {
            st = g_pub.load(std::memory_order_acquire);
            if (static_cast<uint32_t>(st >> 32) != seg) { seg = static_cast<uint32_t>(st >> 32); rd = 0; } // (the game moved to another segment)
            const size_t pub = static_cast<uint32_t>(st);
            static uint64_t seen = ~0ull; // (as vgx_galaxy.cpp PlayerMain: a publication parsed as far as it goes, a cut command left, is not parsed again)
            if (rd < pub && st != seen) {
                seen = st;
                rd += vgx::ProcessStream(g_seg[seg] + rd, static_cast<uint32_t>(pub - rd), false);
                if (rd > pub) { std::fprintf(stderr, "fixture: the player went past what was published (%zu of %zu)\n", rd, pub); std::_Exit(2); }
                g_plRd.store(static_cast<uint32_t>(rd), std::memory_order_release);
                g_plSeen.store(st, std::memory_order_release);
                g_progress.fetch_add(1);
                continue;
            }
            seen = st;
            g_plRd.store(static_cast<uint32_t>(rd), std::memory_order_release);
            g_plSeen.store(st, std::memory_order_release);
            if (g_complete.load(std::memory_order_acquire) >= f && g_endState.load(std::memory_order_acquire) == st) {
                if (rd != pub) { std::fprintf(stderr, "fixture: stream stuck at %zu of %zu\n", rd, pub); std::_Exit(2); }
                break;
            }
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cvPlayer.wait_for(lk, std::chrono::microseconds(500));
        }
        vgx::g.dispCopySrc = vgx::EfbRect{0, 0, 640, 456};
        vgx::QueueCopyDisp(true);
        vgx::FrontPresent(false, nullptr);
        g_finished.store(f, std::memory_order_release);
        Notify(g_cvFinish);
    }
    game.join();
    g_stop.store(true);
    { std::lock_guard<std::mutex> lk(g_wMtx); g_wCv.notify_all(); }
    Notify(g_cvFinish);
    worker.join();
    loading.join();
    watcher.join();
    std::fprintf(stderr, "fe-work fixture: FEW_GAME: %u pictures; the game waited spinning for the worker's view job %llu times; the loading thread "
                         "saw %llu finishes; segment moves %llu; records with alpha bounds %llu\n", frames,
                 static_cast<unsigned long long>(g_jobWaitsSpun.load()), static_cast<unsigned long long>(g_loadWaits.load()),
                 static_cast<unsigned long long>(g_segMoves.load()), static_cast<unsigned long long>(g_abRecords));
    return 0;
}
} // namespace fewgame
namespace {
// ---- FEW_GXDEDUP (gxdedup31): the top-level stream through the game's decision core (port/gx_dedup_core.h), in step with the front end
inline uint32_t DdBe16(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 8) | p[1]; }
inline uint32_t DdBe32(const uint8_t* p) { return (DdBe16(p) << 16) | DdBe16(p + 2); }
// What a list may write, by its content (the front end's vertex sizes now: the list holds no CP command before its draws, or it is "everything")
int DdListClass(uint32_t addr, uint32_t size) {
    if (g_ddClass == 0) return PORT_GXDD_BUMP_ALL;
    addr &= 0x3FFFFFFFu;
    if (addr >= kRam || size > kRam - addr) return PORT_GXDD_BUMP_ALL;
    const uint8_t* p = g_ram + addr;
    const uint8_t* const e = p + size;
    bool regs = false, maskPending = false;
    while (p < e) {
        const uint8_t c = *p;
        if (c == 0x00 || c == 0x44 || c == 0x48) { ++p; continue; }
        if (c == 0x61) {
            if (e - p < 5) return PORT_GXDD_BUMP_ALL;
            regs = true;
            maskPending = p[1] == 0xFE;
            p += 5;
            continue;
        }
        if (c == 0x10) {
            if (e - p < 5) return PORT_GXDD_BUMP_ALL;
            regs = true;
            p += 5 + 4 * ((DdBe16(p + 1) & 0xF) + 1);
            continue;
        }
        if (c == 0x20 || c == 0x28 || c == 0x30 || c == 0x38) { regs = true; p += 5; continue; }
        if (c >= 0x80 && c <= 0xBF) {
            if (e - p < 3) return PORT_GXDD_BUMP_ALL;
            p += 3 + DdBe16(p + 1) * vgx::StreamVertexSize(c & 7);
            continue;
        }
        return PORT_GXDD_BUMP_ALL; // (a CP command, a nested call, anything else)
    }
    if (maskPending) return PORT_GXDD_BUMP_ALL;
    return regs ? PORT_GXDD_BUMP_NOCP : PORT_GXDD_BUMP_PRIMS;
}
// The front end's State without its host pointers (arrays, texture and palette slots: made from the registers and guest memory at a draw)
uint64_t DdStateHash() {
    const vgx::State& g = vgx::g;
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(p); for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull; };
    mix(g.bp, sizeof g.bp); mix(&g.bpMask, 4); mix(g.tevReg, sizeof g.tevReg); mix(g.tevKonst, sizeof g.tevKonst);
    mix(g.xfPos, sizeof g.xfPos); mix(g.xfNrm, sizeof g.xfNrm); mix(g.xfPost, sizeof g.xfPost); mix(g.xfLight, sizeof g.xfLight); mix(g.xfReg, sizeof g.xfReg);
    mix(g.vcd, sizeof g.vcd); mix(g.vat, sizeof g.vat); mix(g.cpBase, sizeof g.cpBase); mix(g.cpStride, sizeof g.cpStride);
    mix(&g.cpArrayStale, 2); mix(&g.bpTexStale, 1); mix(&g.matIdxA, 4); mix(&g.matIdxB, 4);
    mix(g.manualScale, sizeof g.manualScale); mix(&g.manualScaleOn, 1);
    mix(&g.bpDirty, 12); mix(&g.xfPosRowsDirty, 8); mix(&g.tevColDirty, 1); mix(&g.tevOor, 1); mix(&g.ltCos1, 1); mix(&g.ltDist1, 1);
    mix(&g.layoutDirty, 1); mix(&g.vcdGen, 4); mix(&g.texSlotDirty, 1); mix(g.tevRaw, sizeof g.tevRaw); mix(g.cpRaw, sizeof g.cpRaw);
    mix(&g.cpRawValid, 4); mix(&g.keyHash, 8);
    return h;
}
// One picture's top-level stream: decided command by command, fed to the front end up to each draw and list call (an inline draw's
// size needs the vertex format the front end holds there; the State hash is taken there)
void DdFeed(const Bytes& in) {
    Bytes out;
    size_t fed = 0;
    uint64_t sh = 1469598103934665603ull;
    uint32_t syncs = 0;
    auto feed = [&]() {
        while (fed < out.size()) {
            const uint32_t n = vgx::ProcessStream(out.data() + fed, static_cast<uint32_t>(out.size() - fed), false);
            if (!n) { std::fprintf(stderr, "fixture: FEW_GXDEDUP: stream stuck\n"); std::exit(2); }
            fed += n;
        }
    };
    auto sync = [&]() {
        feed();
        if (g_ddState) { sh = (sh ^ DdStateHash()) * 1099511628211ull; ++syncs; }
    };
    auto copy = [&](const uint8_t* p, size_t n) { out.insert(out.end(), p, p + n); };
    auto probe = [&](uint32_t cls, uint32_t reg, uint32_t v) { out.XF(0x0C00, {(cls << 24) | (reg & 0xFFFFu), v}); };
    const uint8_t* p = in.data();
    const uint8_t* const end = p + in.size();
    bool glued = false;
    while (p < end) {
        const uint8_t c = *p;
        if (c != 0x40 && c != 0x5B) glued = false;
        if (c == 0x00 || c == 0x44 || c == 0x48 || c == 0x5B) { copy(p, 1); ++p; continue; }
        if (c == 0x61) {
            const uint32_t rv = DdBe32(p + 1), reg = rv >> 24, kind = rv & 0xFFu;
            if (reg == 0xFF && !((kind >= 21 && kind <= 29) || kind == 38 || kind == 39)) { PortGxDdBump(&g_dds, PORT_GXDD_BUMP_ALL); ++g_ddMarkers; } // (a port marker, as port_vgx_marker)
            const bool same = PortGxDdBp(&g_dds, rv) != 0;
            ++g_ddW[0];
            if (same) ++g_ddSame[0];
            if (!(same && g_dd == 1)) {
                if (same && g_dd == 2) probe(0, reg, rv & 0x00FFFFFFu);
                copy(p, 5);
            }
            p += 5;
            continue;
        }
        if (c == 0x08) {
            const uint32_t reg = p[1], v = DdBe32(p + 2);
            const bool same = PortGxDdCp(&g_dds, reg, v) != 0;
            ++g_ddW[1];
            if (same) ++g_ddSame[1];
            if (!(same && g_dd == 1)) {
                if (same && g_dd == 2) probe(1, reg, v);
                copy(p, 6);
            }
            p += 6;
            continue;
        }
        if (c == 0x10) {
            const uint32_t n = (DdBe16(p + 1) & 0xF) + 1, addr = DdBe16(p + 3);
            uint32_t w[16];
            for (uint32_t i = 0; i < n; ++i) w[i] = DdBe32(p + 5 + 4 * i);
            const bool same = PortGxDdXf(&g_dds, addr, n, w) != 0;
            ++g_ddW[2];
            if (same) ++g_ddSame[2];
            if (!(same && g_dd == 1)) {
                if (same && g_dd == 2) for (uint32_t i = 0; i < n; ++i) probe(2, addr + i, w[i]);
                copy(p, 5 + 4 * n);
            }
            p += 5 + 4 * n;
            continue;
        }
        if (c == 0x20 || c == 0x28 || c == 0x30 || c == 0x38) { copy(p, 5); p += 5; continue; }
        // (a retained handle and the bracket go with the call after them: the front end takes them together; the State is taken before them)
        if (c == 0x59) { sync(); glued = true; copy(p, 5); p += 5; continue; }
        if (c == 0x5A) { sync(); glued = true; copy(p, 21); p += 21; continue; }
        if (c == 0x58) { // (the particle command: TEV colours and list calls inside)
            const uint32_t n = 3 + DdBe16(p + 1);
            copy(p, n);
            p += n;
            PortGxDdBump(&g_dds, PORT_GXDD_BUMP_ALL);
            ++g_ddFx;
            continue;
        }
        if (c == 0x40) {
            if (!glued) sync();
            glued = false;
            const int cls = DdListClass(DdBe32(p + 1), DdBe32(p + 5));
            ++g_ddCalls[cls];
            copy(p, 9);
            p += 9;
            PortGxDdBump(&g_dds, cls);
            continue;
        }
        if (c >= 0x80 && c <= 0xBF) {
            sync();
            const size_t n = 3 + static_cast<size_t>(DdBe16(p + 1)) * vgx::StreamVertexSize(c & 7);
            copy(p, n);
            p += n;
            continue;
        }
        std::fprintf(stderr, "fixture: FEW_GXDEDUP: opcode 0x%02X at top level\n", c);
        std::exit(2);
    }
    feed();
    if (g_ddState) std::printf("dd-state %u: %u syncs %016llx\n", g_frame, syncs, static_cast<unsigned long long>(sh));
}
} // namespace
namespace vgx { uint64_t GxDedupVerifyCounts(uint64_t* bad, uint64_t* digests) __attribute__((weak)); }
// (FEW_VITA_YIELD, with -Wl,--wrap=sched_yield: std::this_thread::yield() does nothing, as in the console's C++ library, whose libstdc++
// is built without _GLIBCXX_USE_SCHED_YIELD; tools/test_vgx_twocore.sh builds the console-like binaries so)
#if defined(FEW_VITA_YIELD)
extern "C" int __wrap_sched_yield(void) { return 0; }
#endif
// (GXV_FE_TWOCORE, vgx_twocore_impl.h: the player raises B while it holds the shared lock, B gives it back; FEW_GAME_RT only)
extern "C" void port_vgx2_boost(int on) {
    if (!fewgame::g_rt || !fewgame::g_bThreadSet) return;
    sched_param sp{};
    sp.sched_priority = on ? fewgame::Fifo(0x8E) : fewgame::g_bPrio;
    pthread_setschedparam(on ? fewgame::g_bThread : pthread_self(), SCHED_FIFO, &sp);
}

int main(int argc, char** argv) {
    const unsigned frames = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 60;
    if (argc > 2) g_rng ^= static_cast<uint64_t>(std::atoll(argv[2])) * 0x9E3779B97F4A7C15ull;
    g_quiet = std::getenv("FEW_PROFILE") != nullptr;
    g_census = std::getenv("FEW_CENSUS") ? std::atoi(std::getenv("FEW_CENSUS")) : 0;
    g_topPct = std::getenv("FEW_TOP") ? std::atoi(std::getenv("FEW_TOP")) : 0;
    g_oddOn = !(std::getenv("FEW_ODD") && std::atoi(std::getenv("FEW_ODD")) == 0);
    g_shStatic = std::getenv("FEW_SHADOW") ? std::atoi(std::getenv("FEW_SHADOW")) : 0;
    g_shDyn = std::getenv("FEW_SHADOWDYN") ? std::atoi(std::getenv("FEW_SHADOWDYN")) : 0;
    g_fxCount = std::getenv("FEW_FX") ? std::atoi(std::getenv("FEW_FX")) : 0;
    g_jpaCount = std::getenv("FEW_JPA") ? std::atoi(std::getenv("FEW_JPA")) : 0;
    g_fxMerge = std::getenv("FEW_FXMERGE") && std::atoi(std::getenv("FEW_FXMERGE")) == 1;
    g_jpaVar = std::getenv("FEW_JPAVAR") && std::atoi(std::getenv("FEW_JPAVAR")) == 1;
    g_retained = std::getenv("FEW_RETAINED") ? std::atoi(std::getenv("FEW_RETAINED")) : -1;
    g_furLayers = std::getenv("FEW_FUR") ? std::atoi(std::getenv("FEW_FUR")) : 0;
    g_plants = std::getenv("FEW_PLANTS") ? std::atoi(std::getenv("FEW_PLANTS")) : 0;
    g_inst = std::getenv("FEW_INST") ? std::atoi(std::getenv("FEW_INST")) : 0;
    g_mixedN = std::getenv("FEW_MIXED") ? std::atoi(std::getenv("FEW_MIXED")) : 0;
    if (std::getenv("FEW_FURN")) g_furCount = std::atoi(std::getenv("FEW_FURN"));
    g_bigCount = std::getenv("FEW_BIG") ? std::atoi(std::getenv("FEW_BIG")) : 0;
    g_j3n = std::getenv("FEW_J3N") && std::atoi(std::getenv("FEW_J3N")) == 1;
    g_diffKeep = std::getenv("FEW_DIFFKEEP") && std::atoi(std::getenv("FEW_DIFFKEEP")) == 1;
    g_miss = std::getenv("FEW_J3N_MISS") && std::atoi(std::getenv("FEW_J3N_MISS")) == 1;
    g_sceneReuse = std::getenv("FEW_SCENE_REUSE") && std::atoi(std::getenv("FEW_SCENE_REUSE")) == 1;
    g_lightEvery = std::getenv("FEW_LIGHT") ? std::atoi(std::getenv("FEW_LIGHT")) : 0;
    g_fogEvery = std::getenv("FEW_FOG") ? std::atoi(std::getenv("FEW_FOG")) : 0;
    g_sceneEvery = std::getenv("FEW_SCENE") ? std::atoi(std::getenv("FEW_SCENE")) : 0;
    g_konstDiff = std::getenv("FEW_KONST") ? std::atoi(std::getenv("FEW_KONST")) : 0;
    g_cam = std::getenv("FEW_CAM") ? std::atoi(std::getenv("FEW_CAM")) : 0;
    g_copyEvery = std::getenv("FEW_COPY") ? std::atoi(std::getenv("FEW_COPY")) : 0;
    if (std::getenv("FEW_GXDEDUP")) { // (gxdedup31)
        g_dd = std::atoi(std::getenv("FEW_GXDEDUP"));
        g_ddClass = std::getenv("FEW_GXDEDUP_CLASS") ? std::atoi(std::getenv("FEW_GXDEDUP_CLASS")) : 1;
        g_ddState = std::getenv("FEW_GXDEDUP_STATE") && std::atoi(std::getenv("FEW_GXDEDUP_STATE")) != 0;
        g_dds.mutant = std::getenv("FEW_GXDEDUP_MUTANT") ? std::atoi(std::getenv("FEW_GXDEDUP_MUTANT")) : 0;
        PortGxDdInit(&g_dds);
    }
    g_ddRand = std::getenv("FEW_GXDEDUP_RAND") ? std::atoi(std::getenv("FEW_GXDEDUP_RAND")) : 0;
    if (std::getenv("FEW_CONSOLE") && std::atoi(std::getenv("FEW_CONSOLE")) != 0) { g_tinyDiff = 6; g_tinyGeo = 4; g_bypassGeo = 2; }
    if (std::getenv("FEW_TINYDIFF")) g_tinyDiff = std::atoi(std::getenv("FEW_TINYDIFF"));
    if (std::getenv("FEW_TINYGEO")) g_tinyGeo = std::atoi(std::getenv("FEW_TINYGEO"));
    if (std::getenv("FEW_BYPASS")) g_bypassGeo = std::atoi(std::getenv("FEW_BYPASS"));
    const size_t trashBytes = std::getenv("FEW_TRASH") ? static_cast<size_t>(std::atoi(std::getenv("FEW_TRASH"))) << 10 : 0;
    // (the PS Vita player's defaults that change the front end's work: vita/vgx-game/entry.cpp; the environment overrides them)
    for (const char* kv : {"GXV_FIX_TEVIN=1", "GXV_FS_HALF=1", "GXV_FIX_LODBIAS=1", "GXV_FOGVTX=2", "GXV_RIGIDMERGE=1", "GXV_STATICCULL=1", "GXV_SOFTXFORM=32",
                           "GXV_TEXBAKE=1", "GXV_PALROWS=1", "GXV_LTMERGE=1", "GXV_FIX_DSTA=1", "GXV_FIX_INDPREV=1", "GXV_FIX_LOGICOP=1", "GXV_FIX_TEXOFF=1",
                           "GXV_TEVWRAP=2", "GXV_DSTALPHA_ZERO=1", "GXV_NODISCARD=1"}) {
        std::string s(kv);
        const size_t eq = s.find('=');
        setenv(s.substr(0, eq).c_str(), s.substr(eq + 1).c_str(), 0);
    }
    if (std::getenv("FEW_GL") && std::atoi(std::getenv("FEW_GL")) != 0) { // (cache24: with a GL back end linked: the production back end draws what the front end hands over, for cachegrind of Draw())
        if (!vgx::host::CreateContext() || !vgx::backend::Init(640, 456)) { std::fprintf(stderr, "fixture: no GL context\n"); return 3; }
    }
    vgx::hle::RegisterGuestRam(g_ram, kRam);
    vgx::GuestHooks hooks{};
    hooks.resolve = Resolve;
    vgx::SetGuestHooks(hooks);
    vgx::ResetState();
    vgx::SetFrameSink(&Observe);
    vgx::g.efbW = 640;
    vgx::g.efbH = 456;
    vgx::QueueSetTarget();
    Build();
    if (g_mixedN > 0) BuildMixed();
    uint64_t bytes = 0;
    const bool gameThreads = std::getenv("FEW_GAME") && std::atoi(std::getenv("FEW_GAME")) != 0;
    if (gameThreads) fewgame::Run(frames, trashBytes, &bytes); // (twocore24: the console's threads; the loop below is then empty)
    for (g_frame = gameThreads ? frames + 1 : 1; g_frame <= frames; ++g_frame) {
        Bytes f = Frame();
        size_t rd = 0, pub = 0;
        // (FEW_FXMERGE set, 0 or 1: the pieces' sizes from a sequence of their own, so that a stream of another length leaves the scene's
        // random values as they were)
        // (GXV_FE_TWOCORE set, 0 or not: the same, since the player handed a segment goes on past pieces it was not handed)
        static const bool ownPieces = std::getenv("FEW_FXMERGE") != nullptr || std::getenv("FEW_RETAINED") != nullptr || std::getenv("FEW_J3N") != nullptr || std::getenv("GXV_FE_TWOCORE") != nullptr;
        static uint64_t pieceRng = 0x9E3779B97F4A7C15ull;
        auto pieceRn = [](uint32_t n) {
            if (!ownPieces) return Rn(n);
            pieceRng ^= pieceRng << 13;
            pieceRng ^= pieceRng >> 7;
            pieceRng ^= pieceRng << 17;
            return static_cast<uint32_t>(pieceRng >> 11) % n;
        };
        // (GXV_FE_TWOCORE: the picture is all there for the second worker, as on the console, where the player is behind the game)
        if (&vgx::two::PublishedUpTo) vgx::two::PublishedUpTo(f.data() + f.size());
        const int64_t feCpu0 = RealTime() ? port_thread_cpu_us() : 0;
        if (g_dd >= 0) { DdFeed(f); rd = f.size(); } // (FEW_GXDEDUP: through the decision core, in step)
        while (rd < f.size()) { // (as the player gets it: in pieces)
            pub = std::min(f.size(), pub + 2048 + pieceRn(4096));
            rd += vgx::ProcessStream(f.data() + rd, static_cast<uint32_t>(pub - rd), false);
            if (rd > pub) pub = rd; // (GXV_FE_TWOCORE: the player took a segment the second worker made, which may end past what it was handed)
            if (pub == f.size() && rd < f.size()) {
                const uint32_t n = vgx::ProcessStream(f.data() + rd, static_cast<uint32_t>(f.size() - rd), false);
                if (!n) { std::fprintf(stderr, "fixture: stream stuck\n"); return 2; }
                rd += n;
            }
        }
        bytes += f.size();
        vgx::g.dispCopySrc = vgx::EfbRect{0, 0, 640, 456};
        vgx::QueueCopyDisp(true);
        vgx::FrontPresent(false, nullptr);
        if (RealTime()) { g_feCpuUs += port_thread_cpu_us() - feCpu0; if (g_frame > frames / 4) { g_feCpuLate += port_thread_cpu_us() - feCpu0; ++g_feLateN; } }
        Mutate(g_frame);
        if (g_mixedN > 0) MutateMixed(g_frame);
        if (trashBytes) {
            static std::vector<uint8_t> trash(trashBytes);
            for (size_t i = 0; i < trash.size(); i += 32) trash[i] = static_cast<uint8_t>(trash[i] + g_frame);
        }
    }
    if (RealTime())
        std::fprintf(stderr, "fe-work fixture: the player's front-end cpu %.3f ms per picture (after the first quarter of the pictures: %.3f ms)\n",
                     g_feCpuUs / 1e3 / frames, g_feLateN ? g_feCpuLate / 1e3 / static_cast<double>(g_feLateN) : 0.0);
    if (std::getenv("FEW_FESPLIT") && vgx::workload::FeSplitOn()) { // (fe24: the FE breakdown slots, GXV_WORKLOAD_PROBE=1 GXV_FE_SPLIT=1: ms and calls per picture; slots nest)
        std::fprintf(stderr, "fe-work fixture: fe-split per picture (FE_SPLIT=1, every scope timed; slots nest, they are not additive):");
        for (unsigned i = 0; i < vgx::workload::FeSlotCount; ++i) {
            const double ms = vgx::workload::FeNs[i].load() / 1e6 / frames, n = vgx::workload::FeN[i].load() / double(frames);
            std::fprintf(stderr, " %s=%.4fms(%.0f)", vgx::workload::FeNames[i], ms, n);
        }
        std::fprintf(stderr, "\n");
    }
    uint64_t sl[5] = {}, sd[5] = {}, sc[3] = {};
    vgx::StaticCullCounters(sc);
    std::fprintf(stderr, "fe-work fixture: static runs tested %llu, culled %llu\n", static_cast<unsigned long long>(sc[0]), static_cast<unsigned long long>(sc[1]));
    if (&vgx::split::FxJoinCounters) {
        uint64_t fj[2] = {};
        vgx::split::FxJoinCounters(fj);
        std::fprintf(stderr, "fe-work fixture: job runs (GXV_FXJOIN): %llu jobs in %llu runs\n", static_cast<unsigned long long>(fj[0]), static_cast<unsigned long long>(fj[1]));
    }
    if (&vgx::FxMergeCounters) {
        uint64_t fx[6] = {};
        vgx::FxMergeCounters(fx);
        std::fprintf(stderr, "fe-work fixture: particle commands (GXV_FXMERGE): %llu commands, %llu items, %llu list calls, %llu short, %llu decodes reused, %llu through CallList\n",
                     static_cast<unsigned long long>(fx[0]), static_cast<unsigned long long>(fx[1]), static_cast<unsigned long long>(fx[2]),
                     static_cast<unsigned long long>(fx[3]), static_cast<unsigned long long>(fx[4]), static_cast<unsigned long long>(fx[5]));
    }
    {
        uint64_t nf[2] = {};
        vgx::NoopFilterCounters(nf);
        std::fprintf(stderr, "fe-work fixture: noop filter (GXV_NOOP_FILTER=2): %llu checked, %llu mismatching\n", static_cast<unsigned long long>(nf[0]), static_cast<unsigned long long>(nf[1]));
    }
    {
        uint64_t sf[4] = {};
        vgx::StateFastCounters(sf);
        std::fprintf(stderr, "fe-work fixture: statefast (GXV_FE_STATEFAST=2): %llu checked, %llu mismatching; %llu commands taken in %llu runs\n", static_cast<unsigned long long>(sf[0]),
                     static_cast<unsigned long long>(sf[1]), static_cast<unsigned long long>(sf[2]), static_cast<unsigned long long>(sf[3]));
    }
    if (&vgx::RetainedCounters) {
        uint64_t rt[7] = {};
        vgx::RetainedCounters(rt);
        std::fprintf(stderr, "fe-work fixture: retained shapes (GXV_RETAINED): %llu calls, %llu hits, %llu without a record, %llu failed a test, %llu recorded; checked %llu, mismatching %llu\n",
                     static_cast<unsigned long long>(rt[0]), static_cast<unsigned long long>(rt[1]), static_cast<unsigned long long>(rt[2]), static_cast<unsigned long long>(rt[3]),
                     static_cast<unsigned long long>(rt[4]), static_cast<unsigned long long>(rt[5]), static_cast<unsigned long long>(rt[6]));
    }
    {
        uint64_t rr[20] = {};
        vgx::RtReasonCounters(rr);
        std::fprintf(stderr, "fe-work fixture: retained reasons (GXV_FE_CENSUS): failed tests: marks %llu, vertex state %llu, list gen %llu, array gen %llu; no record: empty %llu, other shape %llu, "
                             "other list %llu; no record made: no entry %llu, raced %llu, arrays %llu, formats %llu, raw %llu, state %llu; made %llu; GXV_RT_INST: instance slots used %llu, hits %llu, records %llu; verify: entries that differ %llu, vertex bytes differing %llu\n",
                     (unsigned long long)rr[1], (unsigned long long)rr[2], (unsigned long long)rr[3], (unsigned long long)rr[4], (unsigned long long)rr[5], (unsigned long long)rr[6],
                     (unsigned long long)rr[7], (unsigned long long)rr[9], (unsigned long long)rr[10], (unsigned long long)rr[11], (unsigned long long)rr[12], (unsigned long long)rr[13],
                     (unsigned long long)rr[14], (unsigned long long)rr[8], (unsigned long long)rr[15], (unsigned long long)rr[16], (unsigned long long)rr[17], (unsigned long long)rr[19], (unsigned long long)rr[18]);
        char nl[640];
        uint64_t nlost = 0;
        vgx::RtNoneListText(nl, sizeof nl, 1.0, &nlost);
        std::fprintf(stderr, "fe-work fixture: retained, lists of the calls with no record (calls in all):%s\n", nl);
    }
    if (&vgx::DlVariantCounters) {
        uint64_t dv[3] = {}, br[7] = {};
        vgx::DlVariantCounters(dv);
        vgx::DlCacheBranchCounts(br);
        std::fprintf(stderr, "fe-work fixture: list variants (GXV_DL_VARIANTS=%llu): dl-cache hits %llu, bypassed %llu, builds %llu; variant walks verified %llu (differing %llu)\n",
                     static_cast<unsigned long long>(dv[0]), static_cast<unsigned long long>(br[0]), static_cast<unsigned long long>(br[3]),
                     static_cast<unsigned long long>(br[4]), static_cast<unsigned long long>(dv[1]), static_cast<unsigned long long>(dv[2]));
    }
    if (&vgx::fepar::Counters) {
        uint64_t fp[8] = {};
        vgx::fepar::Counters(fp);
        std::fprintf(stderr, "fe-work fixture: decodes on the finishing thread (GXV_FE_PARALLEL): %llu sent (%llu vertices), %llu made on the player, %llu given up; "
                             "checked %llu, mismatching %llu; settles that waited %llu; out-of-range %llu\n",
                     static_cast<unsigned long long>(fp[0]), static_cast<unsigned long long>(fp[1]), static_cast<unsigned long long>(fp[2]),
                     static_cast<unsigned long long>(fp[3]), static_cast<unsigned long long>(fp[4]), static_cast<unsigned long long>(fp[5]),
                     static_cast<unsigned long long>(fp[6]), static_cast<unsigned long long>(fp[7]));
    }
    if (&vgx::ShadowRectFeCounters && &vgx::ShadowRectFeMode && vgx::ShadowRectFeMode() != 0) {
        uint64_t sc[7] = {};
        vgx::ShadowRectFeCounters(sc);
        std::fprintf(stderr, "fe-work fixture: shadow rectangle front end (GXV_SHADOW_RECT_FE=%d): bounds from the memo %llu, worked out %llu (%llu points, %llu repeated vertices skipped), "
                             "deferred key copies not made %llu; compared with the plain code %llu, differing %llu\n",
                     vgx::ShadowRectFeMode(), static_cast<unsigned long long>(sc[0]), static_cast<unsigned long long>(sc[1]), static_cast<unsigned long long>(sc[3]),
                     static_cast<unsigned long long>(sc[2]), static_cast<unsigned long long>(sc[4]), static_cast<unsigned long long>(sc[5]), static_cast<unsigned long long>(sc[6]));
    }
    if (&vgx::DirectJ3DCounters) {
        uint64_t dj[12] = {};
        vgx::DirectJ3DCounters(dj);
        std::fprintf(stderr, "fe-work fixture: direct J3D not armed (fe27): %llu arena lists, %llu learned copies, %llu no runs, %llu switch off or diagnostics\n",
                     static_cast<unsigned long long>(dj[8]), static_cast<unsigned long long>(dj[9]), static_cast<unsigned long long>(dj[10]), static_cast<unsigned long long>(dj[11]));
        std::fprintf(stderr, "fe-work fixture: direct J3D (GXV_DIRECT_J3D): %llu calls armed, %llu runs templated, %llu runs resolved the normal way, %llu key copies deferred, "
                             "%llu made later, %llu templates; verify: %llu checked, %llu mismatching\n",
                     static_cast<unsigned long long>(dj[0]), static_cast<unsigned long long>(dj[1]), static_cast<unsigned long long>(dj[2]), static_cast<unsigned long long>(dj[3]),
                     static_cast<unsigned long long>(dj[4]), static_cast<unsigned long long>(dj[5]), static_cast<unsigned long long>(dj[6]), static_cast<unsigned long long>(dj[7])); // (not "checked N, mismatching M" at the end: tools/test_vgx_retained.sh reads the last such line)
    }
    if (&vgx::J3nCounters) {
        uint64_t jn[24] = {};
        vgx::J3nCounters(jn);
        auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
        std::fprintf(stderr, "fe-work fixture: j3n (GXV_J3DNATIVE): %llu material + %llu differed brackets; stage 1: %llu seen, %llu candidates, skipped %llu (tier A %llu, tier B %llu), "
                             "verified %llu, mismatching %llu, scrubs %llu (bad %llu), declined %llu, no budget %llu; runs static %llu new %llu history %llu ext %llu members %llu animated %llu; "
                             "static draws %llu of %llu J3D draws; new brackets %llu, masked lists %llu, unbracketed shapes %llu, dynamic draws %llu\n",
                     u(jn[0]), u(jn[1]), u(jn[2]), u(jn[3]), u(jn[4] + jn[5]), u(jn[4]), u(jn[5]), u(jn[6]), u(jn[7]), u(jn[8]), u(jn[9]), u(jn[10]), u(jn[11]),
                     u(jn[12]), u(jn[13]), u(jn[14]), u(jn[15]), u(jn[16]), u(jn[17]), u(jn[23]), u(jn[22]), u(jn[18]), u(jn[19]), u(jn[20]), u(jn[21]));
    }
    if (&vgx::J3n2Counters) {
        uint64_t j2[24] = {};
        vgx::J3n2Counters(j2);
        auto u = [](uint64_t v) { return static_cast<unsigned long long>(v); };
        std::fprintf(stderr, "fe-work fixture: j3n stage 2 (GXV_J3DNATIVE=4 / 5): chains started %llu (history differed %llu), recorded %llu, ended as recorded %llu, elsewhere %llu, cut %llu; "
                             "lists lazy %llu, normal %llu; runs templated %llu, bails %llu (not a record hit %llu), captured %llu (refused %llu); lists normal while deviated %llu, animated %llu, deviated shapes %llu; "
                             "blocks sent by templates %llu / %llu / %llu; verify: templates %llu mismatching %llu, patches %llu mismatching %llu; missed writers %llu\n",
                     u(j2[0]), u(j2[1]), u(j2[2]), u(j2[3]), u(j2[4]), u(j2[17]), u(j2[5]), u(j2[6]), u(j2[7]), u(j2[8]), u(j2[19]), u(j2[9]), u(j2[23]), u(j2[10]), u(j2[11]), u(j2[18]),
                     u(j2[20]), u(j2[21]), u(j2[22]), u(j2[12]), u(j2[13]), u(j2[14]), u(j2[15]), u(j2[16]));
    }
    if (&vgx::TevColVerifyCounts) { // (hotprof25: GXV_HOTPROF_TEVCOL=2's markers checked by the front end)
        uint64_t bad = 0;
        const uint64_t n = vgx::TevColVerifyCounts(&bad);
        if (n) std::fprintf(stderr, "fe-work fixture: tevcol verify: %llu checked, %llu differing\n", static_cast<unsigned long long>(n), static_cast<unsigned long long>(bad));
    }
    if (&vgx::sc::ArrVerifyCounts) { // (lighter25: GXV_ARRAYSKIP=2's markers checked by the front end)
        uint64_t bad = 0;
        const uint64_t n = vgx::sc::ArrVerifyCounts(&bad);
        if (n) std::fprintf(stderr, "fe-work fixture: arrayskip verify: %llu checked, %llu differing\n", static_cast<unsigned long long>(n), static_cast<unsigned long long>(bad));
    }
    if (g_dd >= 0) { // (gxdedup31)
        uint64_t bad = 0, dig = 0;
        const uint64_t n = &vgx::GxDedupVerifyCounts ? vgx::GxDedupVerifyCounts(&bad, &dig) : 0;
        std::fprintf(stderr, "fe-work fixture: gxdedup (FEW_GXDEDUP=%d, mutant %d): writes BP %llu CP %llu XF %llu, repeats BP %llu CP %llu XF %llu; list calls: "
                             "everything %llu, BP/XF %llu, primitives %llu; markers %llu, particle commands %llu; verify: %llu checked, %llu differing, %llu digests\n",
                     g_dd, g_dds.mutant, (unsigned long long)g_ddW[0], (unsigned long long)g_ddW[1], (unsigned long long)g_ddW[2], (unsigned long long)g_ddSame[0],
                     (unsigned long long)g_ddSame[1], (unsigned long long)g_ddSame[2], (unsigned long long)g_ddCalls[0], (unsigned long long)g_ddCalls[1],
                     (unsigned long long)g_ddCalls[2], (unsigned long long)g_ddMarkers, (unsigned long long)g_ddFx, (unsigned long long)n, (unsigned long long)bad,
                     (unsigned long long)dig);
    }
    if (&vgx::two::ReportNow) vgx::two::ReportNow(); // (GXV_FE_TWOCORE: the "fetwo:" summary of the run)
    if (g_abRecords || g_awRecords)
        std::fprintf(stderr, "fe-work fixture: records with alpha bounds %llu, writing alpha %llu\n", static_cast<unsigned long long>(g_abRecords),
                     static_cast<unsigned long long>(g_awRecords));
    if (std::getenv("FEW_DETAIL")) { // (fe2-26: what ProcessStream parsed, per picture, to set against the console's "fe stream detail" line)
        uint64_t sd[13] = {};
        vgx::StreamDetailCounters(sd);
        const double n = static_cast<double>(frames);
        std::fprintf(stderr, "fe-work fixture: stream detail per picture: top %.1f streams %.1f KB %.0f state cmds %.0f draws %.0f verts; lists %.0f parsed %.1f KB %.0f state cmds %.0f draws %.0f verts; "
                             "flush strips dropped %.0f; bypass-parsed lists %.1f %.1f KB\n",
                     sd[0] / n, sd[1] / n / 1024.0, sd[2] / n, sd[3] / n, sd[4] / n, sd[5] / n, sd[6] / n / 1024.0, sd[7] / n, sd[8] / n, sd[9] / n, sd[10] / n, sd[11] / n, sd[12] / n / 1024.0);
    }
    if (&vgx::TinyListCounters) { // (tinylist31, GXV_TINYLIST)
        uint64_t tl[8];
        vgx::TinyListCounters(tl);
        std::fprintf(stderr, "fe-work fixture: tinylist: %llu fast replays, %llu replays by StateListCall, %llu refused, %llu backed off; verify: %llu checked, %llu differing, "
                             "not compared %llu + %llu\n",
                     (unsigned long long)tl[0], (unsigned long long)tl[1], (unsigned long long)tl[2], (unsigned long long)tl[3], (unsigned long long)tl[4],
                     (unsigned long long)tl[5], (unsigned long long)tl[6], (unsigned long long)tl[7]);
    }
    if (&vgx::FePlanCounters) { // (feplan31, GXV_FEPLAN)
        uint64_t fp[8] = {};
        vgx::FePlanCounters(fp);
        std::fprintf(stderr, "fe-work fixture: feplan: %llu replays, %llu plans made, %llu refused (vertex format), %llu vertex size changed; verify: %llu plans checked, %llu differing, "
                             "not compared %llu + %llu\n",
                     (unsigned long long)fp[0], (unsigned long long)fp[1], (unsigned long long)fp[2], (unsigned long long)fp[3], (unsigned long long)fp[4],
                     (unsigned long long)fp[5], (unsigned long long)fp[6], (unsigned long long)fp[7]);
    }
    vgx::StateListCounters(sl);
    if (&vgx::SlDeltaCounters) vgx::SlDeltaCounters(sd);
    std::fprintf(stderr, "fe-work fixture: %u frames, %llu stream bytes; state lists: %llu replays, %llu parses; deltas %llu, checked %llu, mismatching %llu; lists compiled %llu, refused %llu\n", frames,
                 static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(sl[0]), static_cast<unsigned long long>(sl[1]),
                 static_cast<unsigned long long>(sd[0]), static_cast<unsigned long long>(sd[1]), static_cast<unsigned long long>(sd[2]),
                 static_cast<unsigned long long>(sd[3]), static_cast<unsigned long long>(sd[4]));
    if (vgx::TexMagEnabled()) { // (GXV_TEXMAG_CENSUS: write the stage's table now, and give the writer thread time to finish it)
        vgx::TexMagSceneEnd();
        usleep(300 * 1000);
    }
    return 0;
}
