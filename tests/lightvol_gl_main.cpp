// GXV_LIGHTVOL host proof, the renderer's half (tools/test_lightvol_gl.sh; as tests/clipblur_gl_main.cpp): the production vgx front end (ProcessStream of the GX
// library's FIFO bytes) and the production GLES back end on Mesa. Per picture it prints the picture's hash and, with LV_DUMPDIR, writes its RGBA8 bytes
// (tools/lightvol_cmp.py compares two runs: bit-identical or PSNR).
#include "vgx_state.h"
#include <GLES2/gl2.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <vector>

extern "C" {
void hg_init(int scene);
void hg_frame(int n);
const uint8_t* hx_resolve(uint32_t a, uint32_t* avail);
void port_log_summary(const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    std::printf("log: ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
}
}

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) {
    if (std::getenv("LV_DRAWS")) {
        unsigned n = 0;
        for (const Cmd& c : b.cmds) n += c.type == CMD_DRAW;
        std::printf("  submit: %u draws\n", n);
    }
    backend::Execute(b);
}
}  // namespace vgx

namespace {
using namespace vgx;
uint64_t Hash(const std::vector<uint8_t>& p) {
    uint64_t h = 14695981039346656037ull;
    for (uint8_t b : p) h = (h ^ b) * 1099511628211ull;
    return h;
}
}  // namespace

extern "C" void hx_fifo_drain(const uint8_t* base, uint8_t** ptr) {
    const uint32_t n = static_cast<uint32_t>(*ptr - base);
    uint32_t done = 0;
    while (done < n) {
        const uint32_t used = ProcessStream(base + done, n - done, false);
        if (used == 0) break;
        done += used;
    }
    if (done < n) std::memmove(const_cast<uint8_t*>(base), base + done, n - done);
    *ptr = const_cast<uint8_t*>(base) + (n - done);
}

int main() {
    const int rw = std::getenv("LV_RW") ? std::atoi(std::getenv("LV_RW")) : 640;
    const int rh = std::getenv("LV_RH") ? std::atoi(std::getenv("LV_RH")) : 456;
    const int frames = std::getenv("LV_FRAMES") ? std::atoi(std::getenv("LV_FRAMES")) : 1;
    const int scene = std::getenv("LV_SCENE") ? std::atoi(std::getenv("LV_SCENE")) : 1;
    if (!host::CreateContext() || !backend::Init(rw, rh)) {
        std::printf("FAIL: no GLES context / back end\n");
        return 1;
    }
    GuestHooks h;
    h.resolve = &hx_resolve;
    SetGuestHooks(h);
    ResetState();
    g.efbW = 640;
    g.efbH = 456;
    QueueSetTarget();
    hg_init(scene);
    int fails = 0;
    for (int n = 1; n <= frames; ++n) {
        hg_frame(n);
        CmdBuf& b = Pending();
        uint32_t readId = 0;
        for (const Cmd& c : b.cmds)
            if (c.type == CMD_COPY_TEX) readId = b.copyTex[c.arg].texId;
        unsigned draws = 0;
        for (const Cmd& c : b.cmds) draws += c.type == CMD_DRAW;
        FrontPresent(false, nullptr);
        std::vector<uint8_t> pic;
        int w = 0, hgt = 0;
        if (!backend::DebugReadTexture(readId, pic, w, hgt) || w <= 0) {
            std::printf("FAIL: frame %d: no read-back\n", n);
            ++fails;
            continue;
        }
        if (const char* dir = std::getenv("LV_DUMPDIR")) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/f%02d-%dx%d.rgba", dir, n, w, hgt);
            if (FILE* f = std::fopen(path, "wb")) {
                std::fwrite(pic.data(), 1, pic.size(), f);
                std::fclose(f);
            }
        }
        std::printf("frame%d hash=%016llx draws=%u\n", n, static_cast<unsigned long long>(Hash(pic)), draws);
    }
    if (glGetError() != GL_NO_ERROR) {
        std::printf("FAIL: GL error\n");
        ++fails;
    }
    return fails ? 1 : 0;
}
