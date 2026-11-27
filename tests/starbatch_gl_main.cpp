// GXV_STARBATCH host proof, the renderer's half (tools/test_starbatch.sh; as tests/lightvol_gl_main.cpp): the production vgx front end (ProcessStream of the GX
// library's FIFO bytes) and the production GLES back end on Mesa. Per picture it prints the picture's hash and the draw records the front end made for it
// (and the front end's CPU time on this machine, for the ratio between runs); with SB_DUMPDIR it writes the picture's RGBA8 bytes.
#include "vgx_state.h"
#include "vgx_hle.h"
#include <GLES2/gl2.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <vector>

extern "C" {
void hg_init(int scene);
void hg_frame(int n);
void hg_report(void);
const uint8_t* hx_resolve(uint32_t a, uint32_t* avail);
void port_log_summary(const char* fmt, ...) {
    std::va_list ap;
    va_start(ap, fmt);
    std::printf("log: ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
}
void hx_guest_written(const void* p, unsigned n) { vgx::hle::NotifyGuestWrite(p, n); }
}

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}  // namespace vgx

namespace {
using namespace vgx;
double gFeSec = 0;
uint64_t Hash(const std::vector<uint8_t>& p) {
    uint64_t h = 14695981039346656037ull;
    for (uint8_t b : p) h = (h ^ b) * 1099511628211ull;
    return h;
}
}  // namespace

extern "C" void hx_fifo_drain(const uint8_t* base, uint8_t** ptr) {
    const uint32_t n = static_cast<uint32_t>(*ptr - base);
    uint32_t done = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (done < n) {
        const uint32_t used = ProcessStream(base + done, n - done, false);
        if (used == 0) break;
        done += used;
    }
    gFeSec += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (done < n) std::memmove(const_cast<uint8_t*>(base), base + done, n - done);
    *ptr = const_cast<uint8_t*>(base) + (n - done);
}

int main() {
    const int rw = 640, rh = 456;
    const int frames = std::getenv("SB_FRAMES") ? std::atoi(std::getenv("SB_FRAMES")) : 8;
    const int scene = std::getenv("SB_SCENE") ? std::atoi(std::getenv("SB_SCENE")) : 1;
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
    double feLast = 0;
    for (int n = 1; n <= frames; ++n) {
        const double fe0 = gFeSec;
        hg_frame(n);
        feLast = gFeSec - fe0;
        CmdBuf& b = Pending();
        uint32_t readId = 0;
        for (const Cmd& c : b.cmds)
            if (c.type == CMD_COPY_TEX) readId = b.copyTex[c.arg].texId;
        size_t records = b.draws.size();
        { size_t nd = 0; for (const Cmd& c : b.cmds) nd += c.type == CMD_DRAW; if (nd > records) records = nd; }
        FrontPresent(false, nullptr);
        std::vector<uint8_t> pic;
        int w = 0, hgt = 0;
        if (!backend::DebugReadTexture(readId, pic, w, hgt) || w <= 0) {
            std::printf("FAIL: frame %d: no read-back\n", n);
            ++fails;
            continue;
        }
        if (const char* dir = std::getenv("SB_DUMPDIR")) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/f%02d-%dx%d.rgba", dir, n, w, hgt);
            if (FILE* f = std::fopen(path, "wb")) {
                std::fwrite(pic.data(), 1, pic.size(), f);
                std::fclose(f);
            }
        }
        unsigned distinct = 0;
        {
            std::vector<uint32_t> seen;
            for (size_t i = 0; i + 3 < pic.size(); i += 4) {
                const uint32_t px = pic[i] | pic[i + 1] << 8 | pic[i + 2] << 16;
                bool f = false;
                for (uint32_t s : seen) if (s == px) { f = true; break; }
                if (!f && seen.size() < 4096) seen.push_back(px);
            }
            distinct = static_cast<unsigned>(seen.size());
        }
        std::printf("frame%d hash=%016llx records=%zu colours=%u fe_ms=%.3f\n", n, static_cast<unsigned long long>(Hash(pic)), records, distinct, feLast * 1000.0);
    }
    hg_report();
    if (glGetError() != GL_NO_ERROR) {
        std::printf("FAIL: GL error\n");
        ++fails;
    }
    return fails ? 1 : 0;
}
