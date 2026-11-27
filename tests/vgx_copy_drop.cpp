// polish23 (8 Nov): GXV_COPY_DROP's front-end part (vgx_texcache.cpp CopyTexturesInRange): the EFB-copy textures made into a destroyed
// scene's heap are counted, and when asked freed: their entries go (a later bind of the address reads guest memory again, a later copy
// gets a new texture), a CMD_TEX_FREE for each goes to the back end, the copies outside the range stay. Real texture cache, null back end.
#include "vgx_state.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"

#include <cstdio>
#include <set>

extern "C" int64_t port_host_time_ns() { return 0; }
extern "C" int64_t port_thread_cpu_us() { return 0; }
extern "C" void port_log(const char*, ...) {}
namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}

static unsigned failures = 0;
static void Require(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

alignas(4096) static unsigned char scene[65536];   // (the scene heap)
alignas(4096) static unsigned char system_[4096];  // (a heap that outlives the scene)

int main() {
    using namespace vgx;
    const uint32_t a = AcquireCopyTexture(scene + 0x100, 320, 228, 0);
    const uint32_t b = AcquireCopyTexture(scene + 0x8000, 160, 114, 0);
    const uint32_t c = AcquireCopyTexture(system_ + 0x40, 640, 456, 0);
    const uint32_t a2 = AcquireCopyTexture(scene + 0x100, 320, 228, 0);  // (the same destination: the same texture)
    Require(a && b && c && a != b && b != c && a == a2, "copy texture ids");
    Require(CopyTextureCount() == 3, "three copy textures");
    const size_t pending0 = Pending().cmds.size();
    // counted only
    Require(CopyTexturesInRange(scene, scene + sizeof scene, false) == 2, "two in the scene heap (count)");
    Require(CopyTextureCount() == 3 && Pending().cmds.size() == pending0, "counting changes nothing");
    // empty / reversed / null ranges
    Require(CopyTexturesInRange(nullptr, nullptr, true) == 0 && CopyTexturesInRange(scene + 16, scene + 16, true) == 0 &&
                CopyTexturesInRange(scene + sizeof scene, scene, true) == 0,
            "empty ranges drop nothing");
    // the end is exclusive
    Require(CopyTexturesInRange(scene, scene + 0x100, true) == 0, "the end of a range is not in it");
    // dropped
    Require(CopyTexturesInRange(scene, scene + sizeof scene, true) == 2, "two dropped");
    Require(CopyTextureCount() == 1, "the system heap's copy stays");
    std::set<uint32_t> freed;
    for (size_t i = pending0; i < Pending().cmds.size(); ++i)
        if (Pending().cmds[i].type == CMD_TEX_FREE) freed.insert(Pending().cmds[i].arg);
    Require(freed.size() == 2 && freed.count(a) && freed.count(b) && !freed.count(c), "a texture free for each dropped copy");
    // a copy into the same address afterwards (the next scene's buffer there) gets a texture of its own
    const uint32_t a3 = AcquireCopyTexture(scene + 0x100, 320, 228, 0);
    Require(a3 && a3 != a && a3 != b && a3 != c, "a new texture for a new copy");
    Require(CopyTextureCount() == 2, "two copy textures again");
    Require(TextureIdsMade() >= 4, "texture ids counted");
    std::printf(failures ? "vgx_copy_drop: FAIL (%u)\n" : "vgx_copy_drop: PASS (counted, dropped in range only, freed, made anew)\n", failures);
    return failures ? 1 : 0;
}
