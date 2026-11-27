// GXV_BLOOM_FAST host proof, the renderer's half (tools/test_bloom_fast_gl.sh): the production vgx front end (vgx::ProcessStream of
// the GX library's FIFO bytes, exactly as platform/src/gx/vgx_galaxy.cpp hands them over) and the production GLES back end on Mesa.
// Per picture it prints the picture's hash, the GXM scenes the back end began (all, and by kind: EFB, copy, pass), and what the bloom's
// part of the picture recorded (commands between the game half's two marks: front-end draws after its merging, texture copies); with
// BF_DUMPDIR the picture's RGBA8 bytes are written for tools/bloom_fast_cmp.py.
#include "vgx_state.h"
#include <GLES2/gl2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
void hg_init(int threshold);
void hg_frame(int n, int glow);
const uint8_t* hx_resolve(uint32_t a, uint32_t* avail);
}

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) {
    if (std::getenv("BF_DUMP")) {
        for (size_t i = 0; i < b.cmds.size(); ++i) {
            const Cmd& c = b.cmds[i];
            if (c.type == CMD_DRAW) {
                const DrawRec& d = b.draws[c.arg];
                std::printf("  %3zu draw: %u idx, ex %d,%d %dx%d, blend %u cw %u aw %u zt %u tex %u\n", i, d.idxCount, d.exX, d.exY, d.exW, d.exH, d.rs.blend,
                            d.rs.colorWrite, d.rs.alphaWrite, d.rs.depthTest, d.texMask ? d.tex[0].texId : 0u);
            } else if (c.type == CMD_COPY_TEX) {
                const CopyTexCmd& t = b.copyTex[c.arg];
                std::printf("  %3zu copy -> %u: src %d,%d %dx%d, dst %ux%u, fromPass %u more %u, skip %u, fill %u\n", i, t.texId, t.src.x, t.src.y, t.src.w, t.src.h, t.dstW,
                            t.dstH, t.fromPass, t.passMore, t.skip, t.fill);
            } else if (c.type == CMD_PASS_BEGIN) {
                const PassCmd& p = b.passes[c.arg];
                std::printf("  %3zu pass begin: %d,%d %dx%d direct %u\n", i, p.rect.x, p.rect.y, p.rect.w, p.rect.h, p.directTex);
            } else {
                std::printf("  %3zu command %u\n", i, static_cast<unsigned>(c.type));
            }
        }
    }
    backend::Execute(b);
}
} // namespace vgx

namespace {
using namespace vgx;
size_t g_mark[2][3];
uint64_t Hash(const std::vector<uint8_t>& p) {
    uint64_t h = 14695981039346656037ull;
    for (uint8_t b : p) h = (h ^ b) * 1099511628211ull;
    return h;
}
uint64_t g_fifoHash = 14695981039346656037ull; // (BF_FIFOHASH: the bloom's GX bytes, for "the same commands as the base")
bool g_inBloom = false;
} // namespace

extern "C" void hx_fifo_drain(const uint8_t* base, uint8_t** ptr) {
    const uint32_t n = static_cast<uint32_t>(*ptr - base);
    if (g_inBloom)
        for (uint32_t i = 0; i < n; ++i) g_fifoHash = (g_fifoHash ^ base[i]) * 1099511628211ull;
    uint32_t done = 0;
    while (done < n) {
        const uint32_t used = ProcessStream(base + done, n - done, false);
        if (used == 0) break;
        done += used;
    }
    if (done < n) std::memmove(const_cast<uint8_t*>(base), base + done, n - done);
    *ptr = const_cast<uint8_t*>(base) + (n - done);
}
extern "C" void hx_mark(int which) {
    CmdBuf& b = Pending();
    size_t draws = 0, copies = 0, passes = 0;
    for (const Cmd& c : b.cmds) {
        draws += c.type == CMD_DRAW;
        copies += c.type == CMD_COPY_TEX;
        passes += c.type == CMD_PASS_BEGIN;
    }
    g_mark[which][0] = draws;
    g_mark[which][1] = copies;
    g_mark[which][2] = passes;
    g_inBloom = which == 0;
}

int main() {
    const int rw = std::getenv("BF_RW") ? std::atoi(std::getenv("BF_RW")) : 720;
    const int rh = std::getenv("BF_RH") ? std::atoi(std::getenv("BF_RH")) : 404;
    const int frames = std::getenv("BF_FRAMES") ? std::atoi(std::getenv("BF_FRAMES")) : 6;
    const int threshold = std::getenv("BF_THRESHOLD") ? std::atoi(std::getenv("BF_THRESHOLD")) : 128;
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
    hg_init(threshold);
    int fails = 0;
    for (int n = 1; n <= frames; ++n) {
        const int glow = (std::getenv("BF_NOGLOW") || n == frames + 1) ? 0 : 1;
        hg_frame(n, glow);
        CmdBuf& b = Pending();
        uint32_t readId = 0;
        for (const Cmd& c : b.cmds)
            if (c.type == CMD_COPY_TEX) readId = b.copyTex[c.arg].texId; // (the last texture copy: the read-back)
        FrontPresent(false, nullptr);
        unsigned kinds[5] = {};
        backend::DebugLastFrameSceneKinds(kinds);
        std::vector<uint8_t> pic;
        int w = 0, hgt = 0;
        if (std::getenv("BF_NOREAD")) {
            unsigned k[5] = {};
            backend::DebugLastFrameSceneKinds(k);
            std::printf("frame%d scenes=%u efb=%u copy=%u pass=%u (no read-back)\n", n, backend::DebugLastFrameScenes(), k[0], k[1], k[2]);
            continue;
        }
        if (!backend::DebugReadTexture(readId, pic, w, hgt) || w <= 0) {
            std::printf("FAIL: frame %d: no read-back\n", n);
            ++fails;
            continue;
        }
        if (const char* dir = std::getenv("BF_DUMPDIR")) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/f%d-%dx%d.rgba", dir, n, w, hgt);
            if (FILE* f = std::fopen(path, "wb")) {
                std::fwrite(pic.data(), 1, pic.size(), f);
                std::fclose(f);
            }
        }
        std::printf("frame%d hash=%016llx scenes=%u efb=%u copy=%u pass=%u hoist=%u disp=%u bloom: draws=%zu copies=%zu\n", n,
                    static_cast<unsigned long long>(Hash(pic)), backend::DebugLastFrameScenes(), kinds[0], kinds[1], kinds[2], kinds[3], kinds[4],
                    g_mark[1][0] - g_mark[0][0], g_mark[1][1] - g_mark[0][1]);
    }
    if (const char* dir = std::getenv("BF_TEXDUMP")) { // (diagnostic: every texture the last picture's copies made)
        for (uint32_t id = 1; id < 24; ++id) {
            std::vector<uint8_t> px;
            int w = 0, hh = 0;
            if (!backend::DebugReadTexture(id, px, w, hh) || w <= 0) continue;
            char path[512];
            std::snprintf(path, sizeof path, "%s/t%u-%dx%d.rgba", dir, id, w, hh);
            if (FILE* f = std::fopen(path, "wb")) { std::fwrite(px.data(), 1, px.size(), f); std::fclose(f); }
        }
    }
    std::printf("bloom-fifo-hash=%016llx efbswaps=%u\n", static_cast<unsigned long long>(g_fifoHash), backend::DebugEfbSwaps());
    if (glGetError() != GL_NO_ERROR) {
        std::printf("FAIL: GL error\n");
        ++fails;
    }
    return fails ? 1 : 0;
}
