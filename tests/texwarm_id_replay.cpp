// texwarm30 integration test: the real texture cache (vgx_texcache.cpp, null back end) with GXV_TEXWARM=2, two visits of one stage whose textures lie at DIFFERENT addresses the second time.
//   usage: texwarm_id_replay key|addr   (key = GXV_TEXWARM_KEY=1, the identity key; addr = the old address-keyed book)
// Visit 1: play, 40 textures first used (recorded).  Between: another stage.  Visit 2: the stage loads, the game's walk reports the textures at their new addresses, the replay runs behind the
// closed screen, then play binds all 40: with the identity key every bind must find its texture already uploaded (0 new uploads in play); with the old key none can be found.
#include "vgx_guestmem.h"
#include "vgx_state.h"

#include <sys/mman.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
namespace hle { void RegisterGuestRam(const void* hostBase, uint32_t size); }
}

static uint32_t g_sceneKey = 0, g_loadKey = 0;
static int g_quiet = 0, g_loading = 0;
static uint32_t g_serial = 1;
static std::string g_log;
extern "C" uint32_t port_scene_key(void) { return g_sceneKey; }
extern "C" uint32_t port_scene_loading_key(void) { return g_loadKey; }
extern "C" int port_screen_quiet(void) { return g_quiet; }
extern "C" int port_scene_loading_active(void) { return g_loading; }
extern "C" uint32_t port_scene_serial(void) { return g_serial; }
extern "C" void port_log_summary(const char* fmt, ...) {
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    g_log += b;
    g_log += "\n";
}
extern "C" void port_texwarm_cand(uint32_t key, uint32_t serial, const void* image, unsigned w, unsigned h, unsigned fmt, unsigned mips, unsigned minFilter, unsigned maxLod8, unsigned order);
extern "C" void port_texwarm_cand_done(uint32_t key, uint32_t serial, unsigned count);

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const unsigned kN = 40, kBytes = 32 * 32 * 2;   // RGB565 32x32
static uint8_t* g_arena = nullptr;

static void FillTex(uint8_t* p, unsigned id) { for (unsigned i = 0; i < kBytes; ++i) p[i] = static_cast<uint8_t>((id * 2654435761u + i * (id | 1u)) >> 5); }

static void Frame() { vgx::TexCacheEndFrame(); }
static unsigned Uploads() { return static_cast<unsigned>(vgx::Pending().uploads.size()); }

static void Bind(const uint8_t* data) {
    vgx::TexSlot& slot = vgx::g.tex[0];
    slot = vgx::TexSlot{};
    slot.valid = true;
    slot.obj.data = data;
    slot.obj.width = slot.obj.height = 32;
    slot.obj.format = 4;   // RGB565
    slot.obj.minFilter = vgx::FILT_LINEAR;
    slot.obj.magFilter = 1;
    slot.avail = 0x100000;
    vgx::g.bpTexStale = 0;
    vgx::TexBind bind;
    uint16_t w = 0, h = 0;
    vgx::ResolveTexture(0, bind, w, h);
}

int main(int argc, char** argv) {
    const bool keyed = argc > 1 && std::string(argv[1]) == "key";
    char dir[] = "/tmp/texwarm_id_XXXXXX";
    if (!mkdtemp(dir)) return 2;
    const std::string book = std::string(dir) + "/book.txt";
    setenv("GXV_TEXWARM", "2", 1);
    setenv("GXV_TEXWARM_KEY", keyed ? "1" : "0", 1);
    setenv("GXV_TEXWARM_FILE", book.c_str(), 1);
    setenv("GXV_CARD_ASYNC", "0", 1);
    // a 32-bit-addressable arena (the cache keys guest pointers as 32-bit)
    g_arena = static_cast<uint8_t*>(mmap(nullptr, 1u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0));
    if (g_arena == MAP_FAILED) { std::printf("no 32-bit arena\n"); return 2; }
    vgx::ResetState();
    vgx::hle::RegisterGuestRam(g_arena, 1u << 20);
    const uint32_t K = 0x4d5f2473u, J = 0x11111111u;
    unsigned addrA[kN], addrB[kN];
    for (unsigned i = 0; i < kN; ++i) { addrA[i] = i * 0x1000u; addrB[i] = 0x80000u + ((i * 7u) % kN) * 0x1000u + 0x20u; }
    // ---- visit 1: the stage's screen opens; its 40 textures are used for the first time in play
    for (unsigned i = 0; i < kN; ++i) FillTex(g_arena + addrA[i], i + 1);
    g_sceneKey = K; g_quiet = 0; g_loading = 0;
    Frame();
    const unsigned base = Uploads();
    for (unsigned i = 0; i < kN; ++i) { Bind(g_arena + addrA[i]); if ((i % 8) == 7) Frame(); }
    CHECK(Uploads() - base == kN, "visit 1: %u first uploads in play, got %u", kN, Uploads() - base);
    // the stage ends, another is played and left
    g_sceneKey = 0; g_quiet = 2; Frame(); Frame();
    g_sceneKey = J; g_quiet = 0; Frame(); Frame();
    g_sceneKey = 0; g_quiet = 2; Frame();
    // ---- visit 2: the same textures, now at other addresses (the stage's archives were loaded elsewhere)
    for (unsigned i = 0; i < kN; ++i) std::memset(g_arena + addrA[i], 0, kBytes);   // (the old places hold other bytes now)
    for (unsigned i = 0; i < kN; ++i) FillTex(g_arena + addrB[i], i + 1);
    g_serial = 2;
    g_loadKey = K; g_loading = 1; g_quiet = 2;
    Frame(); Frame();
    if (keyed) {
        for (unsigned i = 0; i < kN; ++i) port_texwarm_cand(K, 2, g_arena + addrB[i], 32, 32, 4, 1, 1, 0, i);
        port_texwarm_cand_done(K, 2, kN);
    }
    g_loading = 0; g_sceneKey = K; g_loadKey = 0; g_quiet = 2;
    const unsigned beforeClosed = Uploads();
    for (int f = 0; f < 12; ++f) Frame();
    const unsigned madeClosed = Uploads() - beforeClosed;
    g_quiet = 0;
    Frame();
    const unsigned beforePlay = Uploads();
    for (unsigned i = 0; i < kN; ++i) { Bind(g_arena + addrB[i]); if ((i % 8) == 7) Frame(); }
    const unsigned inPlay = Uploads() - beforePlay;
    std::printf("mode %s: uploaded behind the cover %u, first-used in play %u\n", keyed ? "key" : "addr", madeClosed, inPlay);
    if (keyed) {
        CHECK(madeClosed == kN, "identity key: all %u textures uploaded behind the cover, got %u", kN, madeClosed);
        CHECK(inPlay == 0, "identity key: nothing first-used in play, got %u", inPlay);
        CHECK(g_log.find("found 40 by identity") != std::string::npos, "census line says found 40 by identity");
    } else {
        CHECK(madeClosed == 0, "address key: nothing is found at the moved addresses, got %u", madeClosed);
        CHECK(inPlay == kN, "address key: all %u first-used in play, got %u", kN, inPlay);
    }
    // a book file per stage exists (v2) for the identity key
    if (keyed) {
        char path[256];
        std::snprintf(path, sizeof path, "%s.%08x", book.c_str(), K);
        FILE* f = std::fopen(path, "rb");
        CHECK(f != nullptr, "the stage's file %s was written", path);
        if (f) { char head[16] = {0}; std::fread(head, 1, 9, f); std::fclose(f); CHECK(std::strncmp(head, "texwarm 2", 9) == 0, "v2 magic"); }
    }
    // the log (the last census lines)
    size_t pos = g_log.rfind("texwarm: stage");
    if (pos != std::string::npos) std::printf("%s", g_log.c_str() + pos);
    if (g_fail) { std::printf("%d FAILED\n", g_fail); return 1; }
    std::printf("texwarm_id_replay %s: ok\n", keyed ? "key" : "addr");
    return 0;
}
