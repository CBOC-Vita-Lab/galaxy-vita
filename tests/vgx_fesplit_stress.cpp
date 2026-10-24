// Front-end split (vgx_fesplit.h): a synthetic, deterministic workload through the real front end, printing a hash of
// every command buffer the back end would get. Runs with GXV_SPLIT=0 and GXV_SPLIT=1 must print the same lines;
// GXV_SPLIT=2 must report no mismatches. Built with -fsanitize=thread it checks the hand-over between the two threads.
//   vgx_fesplit_stress [frames] [seed]
// (Random register values within what the front end accepts without guest memory: no texture loads, palette loads
// or EFB copy triggers through BP; copies are queued directly, textures are bound as pictures of copies.)
#include "vgx_state.h"
#include "vgx_fesplit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
} // namespace vgx

namespace {
uint64_t g_rng = 88172645463325252ull;
uint32_t Rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return static_cast<uint32_t>(g_rng >> 11);
}
uint32_t Rn(uint32_t n) { return Rnd() % n; }
float Rf(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(Rnd() & 0xFFFFFF) / 16777216.0f; }
uint32_t F2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

uint64_t g_h = 0;
unsigned g_frame = 0;
void Mix(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) g_h = (g_h ^ b[i]) * 1099511628211ull;
}
void Observe(vgx::CmdBuf& b) {
    g_h = 14695981039346656037ull;
    for (const vgx::Cmd& c : b.cmds) { const uint32_t v[2] = {c.type, c.arg}; Mix(v, sizeof v); }
    for (const vgx::DrawRec& d : b.draws) {
        const uint32_t v[16] = {d.vsId, d.fsId, d.prim, d.texMask, d.cover, static_cast<uint32_t>(d.exX), static_cast<uint32_t>(d.exY),
                                static_cast<uint32_t>(d.exW), static_cast<uint32_t>(d.exH), d.layoutMask, d.vbuf, d.vtxOffset, d.vtxCount, d.idxOffset,
                                d.idxCount, d.vtxFirst};
        Mix(v, sizeof v);
        Mix(&d.vchunk, 2);
        Mix(d.ublock, sizeof d.ublock);
        Mix(&d.rs, sizeof d.rs);
        Mix(d.tex, sizeof d.tex);
    }
    Mix(b.vtx.data(), b.vtx.size());
    Mix(b.idx.data(), b.idx.size() * 2);
    Mix(b.uni.data(), b.uni.size() * 4);
    Mix(b.vtxChunks.data(), b.vtxChunks.size() * 4);
    for (const vgx::CopyTexCmd& c : b.copyTex) {
        const uint32_t v[8] = {c.texId, c.skip, c.fromPass, c.fill, c.srcTex, c.dstTexW, static_cast<uint32_t>(c.dstX), c.clear.enable};
        Mix(v, sizeof v);
    }
    for (const vgx::PassCmd& p : b.passes) { const uint32_t v[4] = {p.directTex, p.atlasTex, p.atlasX, p.atlasY}; Mix(v, sizeof v); }
    std::printf("frame %u: %016llx  %zu cmds %zu draws %zu vtx %zu idx %zu uni\n", g_frame, static_cast<unsigned long long>(g_h), b.cmds.size(),
                b.draws.size(), b.vtx.size(), b.idx.size(), b.uni.size());
    vgx::backend::Execute(b);
}

uint8_t g_dest[16][64]; // copy destinations (never read)

void XF1(uint32_t addr, uint32_t v) { vgx::WriteXF(addr, 1, &v); }
void Matrix(uint32_t row) {
    uint32_t w[12];
    for (int i = 0; i < 12; ++i) w[i] = F2u(Rf(-2.0f, 2.0f));
    vgx::WriteXF(row * 4, 12, w);
}
void State() {
    const uint32_t r = Rn(100);
    if (r < 25) Matrix(Rn(62));
    else if (r < 30) { uint32_t w[9]; for (auto& x : w) x = F2u(Rf(-1, 1)); vgx::WriteXF(0x400 + Rn(30) * 3, 9, w); }
    else if (r < 33) { uint32_t w[12]; for (auto& x : w) x = F2u(Rf(-1, 1)); vgx::WriteXF(0x500 + Rn(62) * 4, 12, w); }
    else if (r < 40) XF1(0x1018, (Rn(20) * 3) | (Rn(20) * 3) << 6 | (Rn(20) * 3) << 12);
    else if (r < 44) {
        uint32_t w[16];
        for (int i = 0; i < 16; ++i) w[i] = (i == 3) ? Rnd() : F2u(Rf(-1, 1));
        if (Rn(4) == 0) w[13] = 0x7F800000u; // (a non-finite direction now and then)
        vgx::WriteXF(0x600 + Rn(8) * 16, 16, w);
    } else if (r < 48) XF1(0x100A + Rn(4), Rnd());
    else if (r < 51) XF1(0x100E + Rn(4), Rnd() & 0x7FFF);
    else if (r < 52) XF1(0x1009, Rn(3));
    else if (r < 54) { uint32_t w[7]; for (int i = 0; i < 6; ++i) w[i] = F2u(Rf(-2, 2)); w[6] = Rn(2); vgx::WriteXF(0x1020, 7, w); }
    else if (r < 55) { uint32_t w[6] = {F2u(320), F2u(-228), F2u(16777215), F2u(Rf(300, 400)), F2u(Rf(200, 300)), F2u(16777215)}; vgx::WriteXF(0x101A, 6, w); }
    else if (r < 57) XF1(0x103F, Rn(3));
    else if (r < 59) XF1(0x1040 + Rn(2), (Rn(2) << 1) | (Rn(8) << 7) | (Rn(2) << 4));
    else if (r < 60) XF1(0x1050 + Rn(2), (Rn(20) * 3) | (Rn(2) << 8));
    else if (r < 72) vgx::WriteBP((0xE0u + Rn(8)) << 24 | (Rnd() & 0xFFFFFF));
    else if (r < 78) vgx::WriteBP((0xC0u + Rn(8)) << 24 | (Rnd() & 0xFFFFFF));
    else if (r < 81) vgx::WriteBP(0x00u << 24 | Rn(3) | Rn(3) << 4 | Rn(4) << 10);
    else if (r < 84) vgx::WriteBP((0x28u + Rn(2)) << 24 | (Rnd() & 0xFFFFFF & (Rn(3) ? ~0x40040u : ~0u)));
    else if (r < 87) vgx::WriteBP((0x40u + Rn(3)) << 24 | (Rnd() & 0xFFFFFF));
    else if (r < 88) vgx::WriteBP((0x20u + Rn(2)) << 24 | ((Rn(300) + 342) | (Rn(300) + 342) << 12));
    else if (r < 90) vgx::WriteBP((0xEEu + Rn(5)) << 24 | (Rnd() & 0xFFFFFF));
    else if (r < 91) vgx::WriteBP(0xF3u << 24 | (Rnd() & 0xFFFFFF));
    else if (r < 92) vgx::WriteBP((0xF6u + Rn(4)) << 24 | (Rnd() & 0xFFFFFF));
    else if (r < 93) vgx::SetVcd(0, Rn(4) == 0 ? 1 : 0);
    else if (r < 95) {
        // a texture map that holds a copy's picture
        const int m = static_cast<int>(Rn(2));
        vgx::TexSlot& s = vgx::g.tex[m];
        s.obj = vgx::TexObj{};
        s.obj.data = g_dest[Rn(16)];
        s.obj.width = s.obj.height = static_cast<uint16_t>(16u << Rn(3));
        s.valid = true;
        vgx::g.texDirty = true;
        vgx::g.texSlotDirty |= static_cast<uint8_t>(1u << m);
    } else if (r < 97) {
        vgx::g.texCopySrc = vgx::EfbRect{static_cast<int16_t>(Rn(4) * 64), static_cast<int16_t>(Rn(4) * 64), 64, 64};
        vgx::QueueCopyTex(g_dest[Rn(16)], Rn(2) != 0);
    } else {
        vgx::EmitStaticRun(vgx::PRIM_TRIANGLES, vgx::VL_NRM, 300, 900, 100, 1 + Rn(4), 0, 0, 0);
    }
}

void Draws() {
    static const uint8_t kPrims[] = {0x80, 0x80, 0x80, 0x90, 0x98, 0xA0, 0xA8, 0xB8};
    static const uint32_t kLayouts[] = {0, vgx::VL_NRM, vgx::VL_COL0, vgx::VL_COL0 | vgx::VL_COL1, vgx::VL_TEX0, vgx::VL_NRM | vgx::VL_TEX0 | vgx::VL_COL0,
                                        vgx::VL_TEX0 | (vgx::VL_TEX0 << 1), vgx::VL_COL0 | vgx::VL_TEX0};
    const uint8_t prim = kPrims[Rn(sizeof kPrims)];
    const uint32_t layout = kLayouts[Rn(sizeof kLayouts / 4)];
    const uint32_t run = 1 + Rn(12); // a run of draws that differ in their matrix only (particles)
    uint32_t count = Rn(10) == 0 ? 130 + Rn(300) : (prim == 0x80 ? 4 : 3 + Rn(14));
    const vgx::VtxLayout L = vgx::MakeLayout(layout);
    static uint8_t verts[512 * 112];
    for (uint32_t k = 0; k < run; ++k) {
        if (Rn(3)) Matrix(Rn(20) * 3);
        for (uint32_t i = 0; i < count; ++i) {
            float* p = reinterpret_cast<float*>(verts + i * L.stride);
            p[0] = Rf(-1, 1); p[1] = Rf(-1, 1); p[2] = Rf(-3, -1); p[3] = static_cast<float>(Rn(10) * 3);
            for (uint32_t o = 16; o < L.stride; o += 4) {
                const uint32_t w = Rnd();
                std::memcpy(verts + i * L.stride + o, &w, 4);
            }
            if (layout & vgx::VL_NRM) { float* n = reinterpret_cast<float*>(verts + i * L.stride + L.offNrm); n[0] = Rf(-1, 1); n[1] = Rf(-1, 1); n[2] = Rf(-1, 1); }
            for (int t = 0; t < 8; ++t)
                if (layout & (vgx::VL_TEX0 << t)) { float* c = reinterpret_cast<float*>(verts + i * L.stride + L.offTex[t]); c[0] = Rf(0, 1); c[1] = Rf(0, 1); }
        }
        if (Rn(8) == 0) vgx::WriteBP((0xE2u + Rn(4)) << 24 | (Rnd() & 0xFF07FF)); // a particle's colour
        vgx::DrawDecoded(prim, layout, verts, count);
    }
}
} // namespace

double ThreadCpuMs(clockid_t c) {
    timespec t{};
    clock_gettime(c, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(int argc, char** argv) {
    const unsigned frames = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 200;
    const double cpu0 = ThreadCpuMs(CLOCK_THREAD_CPUTIME_ID), proc0 = ThreadCpuMs(CLOCK_PROCESS_CPUTIME_ID);
    if (argc > 2) g_rng ^= static_cast<uint64_t>(std::atoll(argv[2])) * 0x9E3779B97F4A7C15ull;
    vgx::ResetState();
    vgx::SetFrameSink(&Observe);
    // FESPLIT_LIST=<file>: the shader warm-up list is learnt into this file (it must not exist: compare the files of two runs)
    if (const char* list = std::getenv("FESPLIT_LIST")) vgx::SetShaderListFile(list);
    vgx::g.efbW = 640;
    vgx::g.efbH = 456;
    vgx::QueueSetTarget();
    for (g_frame = 1; g_frame <= frames; ++g_frame) {
        const unsigned ops = 40 + Rn(400);
        for (unsigned i = 0; i < ops; ++i) {
            if (Rn(3) == 0) Draws();
            else State();
        }
        vgx::g.dispCopySrc = vgx::EfbRect{0, 0, 640, 456};
        vgx::QueueCopyDisp(true);
        vgx::FrontPresent(false, nullptr);
    }
    // (stderr: the player's CPU time and the rest of the process's (the finishing thread), for a rough comparison of 0 and 1)
    const double cpu = ThreadCpuMs(CLOCK_THREAD_CPUTIME_ID) - cpu0, proc = ThreadCpuMs(CLOCK_PROCESS_CPUTIME_ID) - proc0;
    int64_t finCpu = 0, waitNs = 0;
    uint32_t splitFrames = 0;
    vgx::split::Counters(finCpu, waitNs, splitFrames);
    std::fprintf(stderr, "cpu: player %.1f ms, other threads %.1f ms; the player caught up with the finishing thread for %.2f ms in %u frames, doing %u operations itself\n",
                 cpu, proc - cpu, waitNs / 1e6, splitFrames, vgx::split::StolenOps());
    return 0;
}
