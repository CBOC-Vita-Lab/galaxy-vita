// bowser24: the repeat-visit slowdown ("the Bowser stage runs at 30 fps the first time and 19 fps on later visits", 760 / 768 soaks) as far as it
// could be this file's own: N "galaxy changes" (the scene key changes, the screen is quiet for a few pictures, every texture the stage uses is at a
// new guest address), each followed by M pictures that make the same scenes (a floor, a GX_CTF_A8 capture read by a quad, an RGB565 capture read by
// a quad, the picture copied out), through the production GX front end and GLES back end on Mesa. The back end's work per picture must not grow with
// the scene changes: scenes per picture, its object counts (programs, texture slots and live textures, pooled render targets) and the median time of
// a picture in the last quarter of the run against the first quarter (software GL: a loose bound). Prints one line per 10 changes.
//   tools/test_vgx_bowser_loop_gl.sh [output-dir]       (BOWSER_SCENES=60 BOWSER_PICS=12 BOWSER_TOL=2.0)
//
// grow24 (the session slowdown: live textures pile up across revisits): extra knobs, all off by default (the base test is unchanged).
//   BOWSER_PRELIVE=N   N extra live textures made before the loop, never freed (as the revisits of 15 visits leave behind)
//   BOWSER_VISIT_TEX=K every scene change loads K new textures (new guest addresses inside the visit's own guest range, as a revisit does)
//   BOWSER_USED=M      every picture binds M of the current visit's textures (the working set)
//   BOWSER_DROP=m      at the end of a visit, the scene-destroy hook for the visit's guest range with GXV_TEX_SCENEDROP mode m (0 = count only; 3 = by age, texage24)
//   BOWSER_CG=1        under callgrind: collect only the measured pictures (CALLGRIND_TOGGLE_COLLECT) and dump after every visit
//   BOWSER_HASH=1      print a hash of the presented picture after each visit (modes 0/1/2 must agree)
//
// slow24 (the copies' cost grows with the live textures on the console): the back end asks vitaGL for the free video memory on the copy path, and
// vitaGL's answer (sceClibMspaceMallocStats) walks every block of its video pool. The desktop answers in no time, so a model stands in for it:
//   BOWSER_VRAMMODEL=1  every question walks a chain of heap nodes, one per live texture (the back end's count, kept per visit) - a stand-in for
//                       the console's walk of the pool's chunk headers; the questions per picture are the production code's own (counted).
//   BOWSER_VRAM_MIB=n   what the model says is free (default 64): below 6 + a target the back end takes its ordinary paths.
//   GXV_SLOW24_VRAMFREE=0/1/2 as on the console; for 1 the test gives the back end an O(1) figure (the same number) in place of vitaGL's patch 0031.
#include "vgx_state.h"
#if __has_include(<valgrind/callgrind.h>)
#include <valgrind/callgrind.h>
#define HAVE_CG 1
#else
#define HAVE_CG 0
#endif
#include <GLES2/gl2.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

static unsigned g_key = 1;
static int g_quiet = 0;
extern "C" uint32_t port_scene_key(void) { return g_key; }
extern "C" int port_screen_quiet(void) { return g_quiet > 0 ? 1 : 0; }
extern "C" uint32_t port_scene_loading_key(void) { return 0; }

namespace vgx::backend::slow24 { // (slow24: the back end's hooks for this test, vgx_backend_gl.cpp)
extern size_t (*g_slowHook)();
extern size_t (*g_fastFn)();
extern uint64_t g_calls;
}
namespace {
struct VramNode { VramNode* next; uint32_t size; uint32_t pad[13]; }; // (a 64-byte "chunk header" each)
VramNode* g_vramChain = nullptr;
size_t g_vramChainN = 0, g_vramMiB = 64;
uint64_t g_vramWalked = 0;
size_t VramModelSlow() { // the walk: every node is read
    size_t sum = 0;
    for (VramNode* n = g_vramChain; n; n = n->next) { sum += n->size; ++g_vramWalked; }
    return (g_vramMiB << 20) + (sum & 0); // (the figure itself does not depend on the walk)
}
size_t VramModelFast() { return g_vramMiB << 20; }
void VramChainTo(size_t n) { // grows the chain to n nodes, scattered over the heap (allocated among other blocks)
    std::vector<void*> spacers;
    while (g_vramChainN < n) {
        spacers.push_back(std::malloc(200));
        VramNode* v = new VramNode{g_vramChain, 64u, {}};
        g_vramChain = v;
        ++g_vramChainN;
    }
    for (void* p : spacers) std::free(p);
}
}
namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}
namespace {
using namespace vgx;
struct Aligned { void* raw; unsigned char* p; };
unsigned failures = 0;
void Check(bool ok, const char* name) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
void XF(unsigned reg, unsigned value) { WriteXF(reg, 1, &value); }
void Viewport() {
    const uint32_t w[] = {f2u(32), f2u(-32), f2u(16777215.0f + 1), f2u(374), f2u(374), f2u(16777215.0f + 1)};
    WriteXF(0x101A, 6, w);
}
void Color(unsigned r, unsigned green, unsigned b, unsigned alpha) {
    BP(0xE2, r | alpha << 12);
    BP(0xE3, b | green << 12);
}
void Pass() { // no texture: colour and alpha = C0
    BP(0x00, 0);
    XF(0x103F, 0);
    BP(0x28, 7u << 7);
    BP(0xC0, (2u << 12) | (15u << 8) | (15u << 4) | 15u | (1u << 19));
    BP(0xC1, (1u << 13) | (7u << 10) | (7u << 7) | (7u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
}
void Quad(float left, float right, float depth, bool tex = false) {
    const float z = depth - 1;
    const float v[] = {left,-1,z,0, 0,1, right,-1,z,0, 1,1, right,1,z,0, 1,0, left,1,z,0, 0,0};
    if (tex) DrawDecoded(0x80, VL_TEX0, reinterpret_cast<const uint8_t*>(v), 4);
    else {
        float plain[16];
        for (int i = 0; i < 4; ++i) std::memcpy(plain + i * 4, v + i * 6, 16);
        DrawDecoded(0x80, 0, reinterpret_cast<const uint8_t*>(plain), 4);
    }
}
void Copy(void* dest, unsigned format) {
    g.texCopySrc = EfbRect{0, 0, 64, 64};
    g.texCopyDstW = g.texCopyDstH = 64;
    g.texCopyFmt = format;
    QueueCopyTex(dest, false);
}
void Bind(void* data, unsigned format) {
    auto& t = g.tex[0];
    t = TexSlot{};
    t.valid = true;
    t.obj.data = data;
    t.obj.width = t.obj.height = 64;
    t.obj.format = format;
    // (grow24: the binding memo is keyed by the texmap's registers, as the game's GXLoadTexObj writes them; the registers must say what g.tex says)
    g.bp[0x88] = 63u | (63u << 10) | (format << 20);
    g.bp[0x94] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data) >> 5) & 0xFFFFFFu;
    g.bpTexStale = 0;
    g.texDirty = true;
    g.texSlotDirty = 1;
    XF(0x103F, 1);
    XF(0x1040, 5u << 7);
    BP(0x00, 1);
    BP(0x28, (1u << 6) | (7u << 7));
}

unsigned char* NewDest(std::vector<std::unique_ptr<unsigned char[]>>& keep) { // 32-aligned, a new address every time
    keep.emplace_back(new unsigned char[96]);
    uintptr_t a = reinterpret_cast<uintptr_t>(keep.back().get());
    return reinterpret_cast<unsigned char*>((a + 31) & ~uintptr_t(31));
}

// ---- grow24: real (guest-memory) textures
static std::vector<unsigned char> g_arena;
static size_t g_arenaOff = 0;
static std::vector<unsigned char*> g_visitTex;      // the current visit's textures
static unsigned g_usedN = 0;
static uint32_t g_seed = 1;
static unsigned char* ArenaTexAt(size_t slot) { // an 8x8 I8 texture (64 bytes) at a fixed place of the arena; its bytes depend on g_seed
    unsigned char* p = g_arena.data() + slot * 64;
    for (unsigned i = 0; i < 64; ++i) p[i] = static_cast<unsigned char>(i * 7 + slot * 13 + g_seed * 29);
    return p;
}
static unsigned char* ArenaTex() { // an 8x8 I8 texture (64 bytes), 64-byte aligned, a new address each time
    if (g_arenaOff + 64 > g_arena.size()) { std::fprintf(stderr, "arena exhausted\n"); std::exit(2); }
    unsigned char* p = g_arena.data() + g_arenaOff;
    g_arenaOff += 64;
    for (unsigned i = 0; i < 64; ++i) p[i] = static_cast<unsigned char>(i * 7 + (g_arenaOff >> 6));
    return p;
}
void BindSized(void* data, unsigned w, unsigned h, unsigned format) {
    auto& t = g.tex[0];
    t = TexSlot{};
    t.valid = true;
    t.obj.data = data;
    t.obj.width = static_cast<uint16_t>(w);
    t.obj.height = static_cast<uint16_t>(h);
    t.obj.format = format;
    // (the binding memo is keyed by the texmap's registers: they say where the image is, as the game's GXLoadTexObj does)
    g.bp[0x88] = (w - 1) | ((h - 1) << 10) | (format << 20);
    g.bp[0x94] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data) >> 5) & 0xFFFFFFu;
    g.bpTexStale = 0;
    g.texDirty = true;
    g.texSlotDirty = 1;
    XF(0x103F, 1);
    XF(0x1040, 5u << 7);
    BP(0x00, 1);
    BP(0x28, (1u << 6) | (7u << 7));
}
void TexState() { // the state of the mask quad in Picture(): one textured, blended draw
    Color(0, 0, 0, 128);
    BP(0xE0, (1u << 23) | (1u << 12));
    BP(0xE1, 1u << 23);
    BP(0xF6, (28u << 9) | (1u << 2));
    BP(0xF7, 2u | (3u << 2));
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 2u | (1u << 19));
    BP(0xC1, (4u << 13) | (6u << 10) | (1u << 7) | (7u << 4) | (3u << 16) | (1u << 19) | (3u << 20));
    BP(0xF3, (4u << 16) | (7u << 19));
    BP(0x40, 0);
    BP(0x41, 1 | (1u << 3) | (1u << 4) | (4u << 8) | (5u << 5));
    BP(0x42, 1u << 8);
}
void DrawReal(unsigned char* const* tex, size_t n) {
    Pass();
    TexState();
    for (size_t i = 0; i < n; ++i) {
        BindSized(tex[i], 8, 8, 1);
        Quad(-1, 1, 0.5f, true);
    }
}
// A picture that shows the visit's textures: 32 strips, strip k sampling texture k (so that it depends on what each texture holds)
uint64_t HashPicture(unsigned char* const* tex, size_t n) {
    Pass();
    BP(0x40, 1 | (7u << 1) | (1u << 4));
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 1u << 8);
    Color(10, 20, 30, 255);
    Quad(-1, 1, 0.5f);
    Pass();
    TexState();
    const size_t strips = std::min<size_t>(n, 32);
    for (size_t k = 0; k < strips; ++k) {
        BindSized(tex[k], 8, 8, 1);
        Quad(-1.0f + 2.0f * float(k) / float(strips), -1.0f + 2.0f * float(k + 1) / float(strips), 0.25f, true);
    }
    FrontPresent(false, nullptr);
    std::vector<unsigned char> px(64 * 64 * 4, 0);
    glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : px) h = (h ^ c) * 1099511628211ull;
    return h;
}
double Median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
void Picture(unsigned char* mask, unsigned char* indirect, unsigned char* read) {
    if (g_usedN && !g_visitTex.empty()) DrawReal(g_visitTex.data(), std::min<size_t>(g_usedN, g_visitTex.size()));
    Pass();
    BP(0x40, 1 | (7u << 1) | (1u << 4));
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 1u << 8);
    Color(40, 200, 90, 255);
    Quad(-1, 0, 0.5f);
    Color(220, 120, 30, 255);
    Quad(0, 1, 0.5f);
    Copy(mask, 0x27);
    Bind(mask, 1);
    Color(0, 0, 0, 128);
    BP(0xE0, (1u << 23) | (1u << 12));
    BP(0xE1, 1u << 23);
    BP(0xF6, (28u << 9) | (1u << 2));
    BP(0xF7, 2u | (3u << 2));
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 2u | (1u << 19));
    BP(0xC1, (4u << 13) | (6u << 10) | (1u << 7) | (7u << 4) | (3u << 16) | (1u << 19) | (3u << 20));
    BP(0xF3, (4u << 16) | (7u << 19));
    BP(0x40, 0);
    BP(0x41, 1 | (1u << 3) | (1u << 4) | (4u << 8) | (5u << 5));
    BP(0x42, 1u << 8);
    Quad(-1, 1, 0.5f, true);
    Copy(indirect, 4);
    Bind(indirect, 4);
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19));
    BP(0xC1, (7u << 13) | (7u << 10) | (7u << 7) | (1u << 4) | (1u << 19));
    BP(0xF3, (7u << 16) | (7u << 19));
    BP(0x40, 0);
    BP(0x41, (1u << 3) | (1u << 4));
    BP(0x42, 0);
    Quad(0.25f, 1, 0.5f, true);
    Copy(read, 6);
}

// ---- texage24: GXV_TEX_SCENEDROP=3 (age and cap), the refetch census and GXV_TEX_ADOPT, on the production front end and GLES back end.
// A two-stage loop as the c35 console run: stage A and stage B alternate; each visit's first picture loads the stage's K textures (8x8 I8, bytes
// depend on the stage and the texture index only) and every picture binds all of them; the visit ends with the scene-destroy hook
// (TextureCacheSceneGone, mode = GXV_TEX_SCENEDROP). Knobs (AGE_SCENARIO=1 switches this on):
//   AGE_VISITS=n [12]  AGE_TEX=K [40]  AGE_PICS=n [40] pictures a stage visit draws   AGE_FRESH=1 each visit loads at NEW addresses (the same bytes)
//   AGE_SHORT=n [0]    a short scene of n pictures (binding 3 textures of its own) between every two stage visits
//   AGE_QUIET=n [0]    quiet pictures at the start of a visit (the textures are then loaded in a quiet picture, not in play)
//   AGE_HEAP=1         the destroyed scene's heap range handed to the hook is the visit's own region (the rule of mode 1)
//   AGE_STAGES=n [2]   stages in the loop
// Prints "age-visit" lines and one "AGE-RESULT" line: uploads (after the first loop), the cache, the census, and a hash of the pictures.
static unsigned g_ageUploads = 0;
static void AgeUploadHook(const TexUpload&, uint32_t, uint64_t) { ++g_ageUploads; }
static int EnvIntOr(const char* n, int d) { const char* v = std::getenv(n); return v ? std::atoi(v) : d; }
int AgeScenario() {
    const int visits = EnvIntOr("AGE_VISITS", 12), K = EnvIntOr("AGE_TEX", 40), pics = EnvIntOr("AGE_PICS", 40), stages = EnvIntOr("AGE_STAGES", 2);
    const int shortN = EnvIntOr("AGE_SHORT", 0), quiet = EnvIntOr("AGE_QUIET", 0);
    const bool fresh = EnvIntOr("AGE_FRESH", 0) != 0, heap = EnvIntOr("AGE_HEAP", 0) != 0;
    const int mode = TexSceneDropMode();
    g_arena.assign(static_cast<size_t>(64) * (static_cast<size_t>(stages) * K * (fresh ? visits + 1 : 1) + 64 * (visits + 2)), 0);
    SetTextureDumpHook(AgeUploadHook);
    auto fill = [](unsigned char* p, int stage, int k) { for (unsigned i = 0; i < 64; ++i) p[i] = static_cast<unsigned char>(i * 7 + k * 13 + stage * 101); };
    std::vector<unsigned char*> fixedTex[8];
    size_t off = 0;
    for (int st = 0; st < stages && !fresh; ++st)
        for (int k = 0; k < K; ++k) { unsigned char* p = g_arena.data() + off; off += 64; fill(p, st, k); fixedTex[st].push_back(p); }
    std::vector<unsigned char> mask(96), indirect(96), readb(96);
    uint64_t picHash = 1469598103934665603ull;
    unsigned uploadsAfterWarm = 0, maxCache = 0, adoptedLast = 0;
    unsigned cacheAfter = 0;
    TexSceneDrop last;
    const int total = visits * (shortN ? 2 : 1);
    for (int v = 0; v < total; ++v) {
        const bool isShort = shortN && (v & 1);
        const int st = (shortN ? v / 2 : v) % stages;
        std::vector<unsigned char*> tex;
        unsigned char* lo = g_arena.data() + off;
        if (isShort) {
            for (int k = 0; k < 3; ++k) { unsigned char* p = g_arena.data() + off; off += 64; fill(p, 90 + v, k); tex.push_back(p); }
        } else if (fresh) {
            for (int k = 0; k < K; ++k) { unsigned char* p = g_arena.data() + off; off += 64; fill(p, st, k); tex.push_back(p); }
        } else {
            tex = fixedTex[st];
        }
        unsigned char* hi = g_arena.data() + off;
        ++g_key;
        g_quiet = isShort ? 0 : quiet;
        const unsigned up0 = g_ageUploads;
        const int n = isShort ? shortN : pics;
        for (int i = 0; i < n; ++i) {
            DrawReal(tex.data(), tex.size());
            FrontPresent(false, nullptr);
            if (g_quiet > 0) --g_quiet;
        }
        g_quiet = 0;
        if (!isShort) picHash = (picHash ^ HashPicture(tex.data(), tex.size())) * 1099511628211ull;
        const unsigned visitUploads = g_ageUploads - up0;
        const void* lov[1] = {heap ? static_cast<const void*>(lo) : nullptr};
        const void* hiv[1] = {heap ? static_cast<const void*>(hi) : nullptr};
        last = TextureCacheSceneGone(lov, hiv, 1, mode);
        cacheAfter = static_cast<unsigned>(TextureCacheEntries());
        maxCache = std::max(maxCache, last.cacheBefore);
        if (v >= 2 * stages * (shortN ? 2 : 1)) uploadsAfterWarm += visitUploads; // (after two loops: caches are warm)
        adoptedLast = last.adopted;
        std::printf("age-visit %2d %s%d: uploads %3u, cache %u -> %u (aged %u, over caps %u, in heap %u, play pictures %u%s), refetch addr %u (%u live) content %u (%u live), adopted %u\n", v,
                    isShort ? "short" : "stage", isShort ? 0 : st, visitUploads, last.cacheBefore, cacheAfter, last.aged, last.freedCap, last.inRange, last.playPics,
                    last.shortScene ? ", short" : "", last.refAddr, last.refAddrLive, last.refContent, last.refContentLive, last.adopted);
    }
    std::printf("AGE-RESULT mode=%d uploads_warm=%u cache_end=%u cache_max=%u live_kib=%u refAddr=%u refAddrLive=%u refContent=%u refContentLive=%u adopted=%u hash=%016llx\n", mode, uploadsAfterWarm,
                cacheAfter, maxCache, static_cast<unsigned>(last.liveBytes / 1024u), last.refAddr, last.refAddrLive, last.refContent, last.refContentLive, adoptedLast, static_cast<unsigned long long>(picHash));
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
}

int main() {
    const bool vramModel = std::getenv("BOWSER_VRAMMODEL") && std::atoi(std::getenv("BOWSER_VRAMMODEL")) != 0;
    if (std::getenv("BOWSER_VRAM_MIB")) g_vramMiB = static_cast<size_t>(std::atoi(std::getenv("BOWSER_VRAM_MIB")));
    if (vramModel) {
        backend::slow24::g_slowHook = VramModelSlow;
        backend::slow24::g_fastFn = VramModelFast; // (used by GXV_SLOW24_VRAMFREE=1 only)
    }
    Check(host::CreateContext(), "GLES context");
    Check(backend::Init(64, 64), "production backend init");
    if (failures) return 1;
    ResetState();
    g.efbW = g.efbH = 64;
    QueueSetTarget();
    BP(0x43, 1);
    BP(0x20, 342u | (342u << 12));
    BP(0x21, 405u | (405u << 12));
    BP(0x59, 171u | (171u << 10));
    const uint32_t m[] = {f2u(1),0,0,0, 0,f2u(1),0,0, 0,0,f2u(1),0};
    WriteXF(0, 12, m);
    const uint32_t proj[] = {f2u(1),0,f2u(1),0,f2u(1),0,1};
    WriteXF(0x1020, 7, proj);
    Viewport();
    if (std::getenv("AGE_SCENARIO")) return AgeScenario();
    const int scenesN = std::getenv("BOWSER_SCENES") ? std::atoi(std::getenv("BOWSER_SCENES")) : 60;
    const int picsN = std::getenv("BOWSER_PICS") ? std::atoi(std::getenv("BOWSER_PICS")) : 12;
    const double tol = std::getenv("BOWSER_TOL") ? std::atof(std::getenv("BOWSER_TOL")) : 2.0;
    std::vector<std::unique_ptr<unsigned char[]>> keep;
    std::vector<double> us;
    std::vector<unsigned> sceneCounts;
        const bool fresh = std::getenv("BOWSER_FRESH") && std::atoi(std::getenv("BOWSER_FRESH")) != 0;
    std::vector<unsigned char*> ring[4];
    unsigned programs0 = 0, slots0 = 0, live0 = 0, pooled0 = 0, programs = 0, slots = 0, live = 0, pooled = 0;
    const int preLive = std::getenv("BOWSER_PRELIVE") ? std::atoi(std::getenv("BOWSER_PRELIVE")) : 0;
    const int visitTex = std::getenv("BOWSER_VISIT_TEX") ? std::atoi(std::getenv("BOWSER_VISIT_TEX")) : 0;
    g_usedN = std::getenv("BOWSER_USED") ? static_cast<unsigned>(std::atoi(std::getenv("BOWSER_USED"))) : 0;
    const int dropMode = std::getenv("BOWSER_DROP") ? std::atoi(std::getenv("BOWSER_DROP")) : -1;
    const bool cg = std::getenv("BOWSER_CG") && std::atoi(std::getenv("BOWSER_CG")) != 0;
    const bool hashPic = std::getenv("BOWSER_HASH") && std::atoi(std::getenv("BOWSER_HASH")) != 0;
    const int ringN = std::getenv("BOWSER_RING") ? std::atoi(std::getenv("BOWSER_RING")) : 0;  // >0: the visits' textures reuse ringN regions of memory
    const bool sameBytes = std::getenv("BOWSER_SAME") && std::atoi(std::getenv("BOWSER_SAME")) != 0;
    uint64_t picHash = 1469598103934665603ull;
    const int rtbN = std::getenv("BOWSER_RTB") ? std::atoi(std::getenv("BOWSER_RTB")) : 0; // calls of the render-target census between visits
    g_arena.assign(static_cast<size_t>(64) * (static_cast<size_t>(preLive) + static_cast<size_t>(visitTex) * (static_cast<size_t>(std::max(scenesN + 2, ringN + 1))) + 64), 0);
    if (preLive > 0) { // old visits: textures made and used once, a few per picture, then left in the cache
        std::vector<unsigned char*> batch;
        for (int i = 0; i < preLive; ++i) {
            batch.push_back(ArenaTex());
            if (batch.size() == 100 || i == preLive - 1) { DrawReal(batch.data(), batch.size()); FrontPresent(false, nullptr); batch.clear(); }
        }
        unsigned a, b, c, d;
        backend::DebugObjectCounts(a, b, c, d);
        std::printf("pre-live: %d textures made; back end live %u slots %u\n", preLive, c, b);
    }
    for (int s = 0; s < scenesN; ++s) {
        ++g_key;
        unsigned char* visitLo = g_arena.data() + g_arenaOff;
        g_visitTex.clear();
        if (ringN > 0) { // the guest reuses its memory: visit s lays its textures out in region s % ringN, with the bytes of its own seed (or the same ones: BOWSER_SAME=1)
            g_seed = sameBytes ? 1u : static_cast<uint32_t>(s + 1);
            for (int k = 0; k < visitTex; ++k) g_visitTex.push_back(ArenaTexAt(static_cast<size_t>(preLive) + static_cast<size_t>(s % ringN) * visitTex + k));
        } else {
            for (int k = 0; k < visitTex; ++k) g_visitTex.push_back(ArenaTex());
        }
        unsigned char* visitHi = g_arena.data() + g_arenaOff;
        g_quiet = 3; // (a few quiet pictures after a change: GXV_SHADER_RECLAIM's moment)
        if (keep.size() > 60) keep.erase(keep.begin(), keep.begin() + 30); // (the stage's old buffers go: the guest frees them)
        // (the guest reuses its memory: three buffers per stage from a ring of four stages; BOWSER_FRESH=1: a new address every change, as if no
        // texture were ever freed - then the live textures climb by design, and the time per picture against them is what is printed)
        if (fresh) { ring[s % 4].clear(); }
        if (ring[s % 4].empty()) for (int k = 0; k < 3; ++k) ring[s % 4].push_back(NewDest(keep));
        unsigned char* mask = ring[s % 4][0];
        unsigned char* indirect = ring[s % 4][1];
        unsigned char* read = ring[s % 4][2];
        if (vramModel) { unsigned a, b, c, d; backend::DebugObjectCounts(a, b, c, d); VramChainTo(c); } // (slow24: one block per live texture)
        const uint64_t asks0 = backend::slow24::g_calls, walked0 = g_vramWalked;
        for (int i = 0; i < picsN; ++i) {
            if (i == 0 && !g_visitTex.empty()) { // the visit's first picture loads its textures
                const size_t before = TextureCacheEntries();
                DrawReal(g_visitTex.data(), g_visitTex.size());
                if (std::getenv("BOWSER_EVERY")) std::printf("  visit %d: cache %zu -> %zu after the draws\n", s, before, TextureCacheEntries());
                FrontPresent(false, nullptr);
            }
#if HAVE_CG
            if (cg && s > 4 && i == 2) CALLGRIND_START_INSTRUMENTATION;
#endif
            Picture(mask, indirect, read);
            const auto t0 = std::chrono::steady_clock::now();
            FrontPresent(false, nullptr);
#if HAVE_CG
            if (cg && s > 4 && i == picsN - 1) CALLGRIND_STOP_INSTRUMENTATION;
#endif
            const double took = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            if (hashPic && i == picsN - 1) { // (slow24: the copies' own picture - the captures read back by the quads - in the hash too)
                std::vector<unsigned char> px(64 * 64 * 4, 0);
                glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                for (unsigned char c : px) picHash = (picHash ^ c) * 1099511628211ull;
            }
            if (std::getenv("BOWSER_EVERY") && s < 8) std::printf("   s %d pic %d: cache %zu quiet %d\n", s, i, TextureCacheEntries(), g_quiet);
            if (g_quiet > 0) --g_quiet;
            if (s > 4) { // (the first stages compile their shaders and fill the ring)
                us.push_back(took);
                const unsigned n = backend::DebugLastFrameScenes();
                sceneCounts.push_back(n);
            } // (the first stage compiles its shaders)
        }
        if (hashPic && !g_visitTex.empty()) {
            const uint64_t h = HashPicture(g_visitTex.data(), g_visitTex.size());
            picHash = (picHash ^ h) * 1099511628211ull;
            if (std::getenv("BOWSER_EVERY")) std::printf("  visit %d: picture hash %016llx\n", s, static_cast<unsigned long long>(h));
        }
        if (ringN > 0 && !g_visitTex.empty()) { visitLo = g_visitTex.front(); visitHi = g_visitTex.back() + 64; }
        if (dropMode >= 0 && !g_visitTex.empty()) { // (texage24: called directly; modes 0..3)
            const void* lo[1] = {visitLo};
            const void* hi[1] = {visitHi};
            TextureCacheSceneGone(lo, hi, 1, dropMode);
        }
        (void)visitLo; (void)visitHi; (void)dropMode;
#if HAVE_CG
        if (cg && s > 4) { char nm[32]; std::snprintf(nm, sizeof nm, "visit %d", s); CALLGRIND_DUMP_STATS_AT(nm); CALLGRIND_ZERO_STATS; }
#endif
        if (rtbN > 0 && s > 4) { // (grow24) the render targets' memory, as the quality controller and the logs ask for it: a walk of the texture table at the base
#if HAVE_CG
            if (cg) CALLGRIND_START_INSTRUMENTATION;
#endif
            uint64_t sink = 0;
            for (int k = 0; k < rtbN; ++k) { unsigned a; uint64_t b, c; backend::DebugPoolStats(a, b, c); sink += b; }
#if HAVE_CG
            if (cg) { CALLGRIND_STOP_INSTRUMENTATION; char nm[32]; std::snprintf(nm, sizeof nm, "rtb %d", s); CALLGRIND_DUMP_STATS_AT(nm); CALLGRIND_ZERO_STATS; }
#endif
            if (std::getenv("BOWSER_EVERY")) std::printf("  rtb sink %llu\n", static_cast<unsigned long long>(sink));
        }
        backend::DebugObjectCounts(programs, slots, live, pooled);
        if (vramModel && (s % 10 == 0 || s == scenesN - 1 || std::getenv("BOWSER_EVERY")))
            std::printf("  slow24: change %3d: free-video-memory questions %.2f a picture, model blocks %zu, blocks read %.0f a picture\n", s,
                        double(backend::slow24::g_calls - asks0) / picsN, g_vramChainN, double(g_vramWalked - walked0) / picsN);
        if (s == 5) { programs0 = programs; slots0 = slots; live0 = live; pooled0 = pooled; }
        if (s % 10 == 0 || s == scenesN - 1 || std::getenv("BOWSER_EVERY"))
            std::printf("change %3d: (front end: ids %u cache %zu) programs %u texture slots %u live %u pooled targets %u, last picture %.0f us\n", s, TextureIdsMade(), TextureCacheEntries(), programs, slots, live, pooled,
                        us.empty() ? 0.0 : us.back());
    }
    if (us.size() < 8) return 1;
    const size_t q = us.size() / 4;
    const double first = Median(std::vector<double>(us.begin(), us.begin() + q));
    const double last = Median(std::vector<double>(us.end() - q, us.end()));
    const unsigned sMin = *std::min_element(sceneCounts.begin(), sceneCounts.end()), sMax = *std::max_element(sceneCounts.begin(), sceneCounts.end());
    std::printf("pictures %zu: median first quarter %.0f us, last quarter %.0f us (ratio %.2f); scenes per picture %u..%u\n", us.size(), first, last,
                first > 0 ? last / first : 0.0, sMin, sMax);
    // (the EFB swap's adoption makes a picture 4 or 5 scenes depending on which buffer a copy lands in: not a trend; a trend is what is checked)
    auto mean = [&](size_t from, size_t to) { double t = 0; for (size_t k = from; k < to; ++k) t += sceneCounts[k]; return t / double(to - from); };
    const double sFirst = mean(0, q), sLast = mean(sceneCounts.size() - q, sceneCounts.size());
    std::printf("mean scenes per picture: first quarter %.2f, last quarter %.2f\n", sFirst, sLast);
    Check(sLast <= sFirst + 0.25, "scenes per picture do not grow with the changes");
    const bool grow = preLive > 0 || visitTex > 0; // (grow24 runs: live textures climb by design)
    if (!grow) {
    Check(programs == programs0, "program count flat");
    Check(pooled == pooled0, "pooled render targets flat");
    Check(fresh || live <= live0 + 8, "live textures flat (the ring's buffers are reused)");
    Check(slots <= slots0 + 8 + static_cast<unsigned>(scenesN) * 3 / 4, "texture slots do not grow without a bound");
    }
    if (hashPic) std::printf("picture hash over %d visits: %016llx\n", scenesN, static_cast<unsigned long long>(picHash));
    if (fresh) std::printf("BOWSER_FRESH: live textures %u -> %u, picture time %.0f -> %.0f us\n", live0, live, first, last);
    Check(fresh || grow || last <= first * tol + 300.0, "median back-end time of a picture is flat (last quarter <= first quarter * BOWSER_TOL + 0.3 ms; not checked with BOWSER_FRESH)");
    Check(glGetError() == GL_NO_ERROR, "no GL error");
    return failures ? 1 : 0;
}
