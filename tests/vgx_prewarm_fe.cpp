// GXV_PREWARM end to end (tools/test_vgx_prewarm.sh): the front-end half. The game half (tests/vgx_prewarm.cpp) drives the real GX
// library units and the real gx_fifo.cpp; what the FIFO hands over is parsed here by the real front end in pieces of random size, and
// every picture is printed as a canonical hash: the draw records with the shader ids replaced by their keys' hashes and the texture ids
// by their uploads' contents (a warm-up scope makes shaders and uploads earlier, so their ids differ; what they stand for must not), the
// vertices, indices and uniform blocks, and every command but the shader / program / variant / upload / buffer ones. With PW=1 these
// lines must equal PW=0's (tools/test_vgx_prewarm.sh: nothing of the scopes reaches a picture).
// fe_report (PW=1): (a) every variant a real draw is recorded with after the closed frames - (vertex key, fragment key, layout, blend key)
// - was queued by a warm-up scope, and no shader is made after the closed frames (no new key at the real draws).
#include "vgx_state.h"
#include "vgx_fesplit.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <vector>

extern "C" int64_t port_host_time_ns() { static std::atomic<int64_t> n{1}; return n.fetch_add(1000); }
extern "C" int64_t port_thread_cpu_us() { static std::atomic<int64_t> n{1}; return n.fetch_add(1); }
extern "C" void port_log_summary(const char* fmt, ...) { // (PW_LOGSUM=1: the front end's log lines to stderr, tests/vgx_layoutwarm.cpp reads the census lines)
    if (!std::getenv("PW_LOGSUM")) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}
extern "C" void port_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
} // namespace vgx

namespace {
constexpr uint32_t kRam = 1u << 20;
alignas(4096) uint8_t g_ram[kRam];
uint32_t g_brk = 0x1000;
const uint8_t* Resolve(uint32_t address, uint32_t* avail) {
    *avail = 0;
    address &= 0x3FFFFFFFu;
    if (address == 0 || address >= kRam) return nullptr;
    *avail = kRam - address;
    return g_ram + address;
}
uint64_t g_h = 0;
unsigned g_frame = 0;
uint64_t g_pieceRng = 0x2545F4914F6CDD1Dull;
uint32_t PieceRn(uint32_t n) {
    g_pieceRng ^= g_pieceRng << 13;
    g_pieceRng ^= g_pieceRng >> 7;
    g_pieceRng ^= g_pieceRng << 17;
    return static_cast<uint32_t>(g_pieceRng >> 11) % n;
}
void Mix(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) g_h = (g_h ^ b[i]) * 1099511628211ull;
}
uint64_t HashOf(const void* p, size_t n) {
    uint64_t h = 14695981039346656037ull;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
std::map<uint32_t, uint64_t> g_vsKey, g_fsKey, g_texKey; // id -> what it stands for
using Var = std::tuple<uint64_t, uint64_t, uint32_t, uint32_t>;
std::set<Var> g_warmVars;
std::vector<std::pair<unsigned, Var>> g_realVars; // (frame, variant) of every record
std::map<unsigned, unsigned> g_shadersMade;      // frame -> CMD_SHADER count
std::set<uint64_t> g_lateKeys;                   // keys of the shaders made after the closed frames
unsigned g_closedFrames = 0;
uint64_t g_warmCmds = 0;

void Observe(vgx::CmdBuf& b) {
    for (const vgx::Cmd& c : b.cmds)
        if (c.type == vgx::CMD_SHADER) {
            const vgx::ShaderSource* s = b.shaders[c.arg];
            (s->fragment ? g_fsKey : g_vsKey)[s->id] = s->keyHash;
            ++g_shadersMade[g_frame];
            if (g_closedFrames && g_frame > g_closedFrames) g_lateKeys.insert(s->keyHash);
            if (std::getenv("PW_DEBUG")) std::fprintf(stderr, "prewarm test: frame %u: %s shader %u key %016llx\n%s\n", g_frame, s->fragment ? "fragment" : "vertex", s->id,
                                                     static_cast<unsigned long long>(s->keyHash), std::atoi(std::getenv("PW_DEBUG")) > 1 ? s->text.c_str() : "");
        }
    for (const vgx::Cmd& c : b.warm)
        if (c.type == vgx::CMD_SHADER) {
            const vgx::ShaderSource* s = b.shaders[c.arg];
            (s->fragment ? g_fsKey : g_vsKey)[s->id] = s->keyHash;
        }
    for (const vgx::TexUpload& u : b.uploads) {
        uint64_t h = HashOf(&u.width, sizeof u.width) ^ (HashOf(&u.height, sizeof u.height) << 1) ^ (static_cast<uint64_t>(u.format) << 3);
        for (const auto& l : u.levels) h = h * 31 + HashOf(l.data(), l.size());
        g_texKey[u.texId] = h;
    }
    auto noteWarm = [&](const vgx::Cmd& c) {
        if (c.type != vgx::CMD_VARIANT || !(c.arg & 0x80000000u)) return;
        const vgx::VariantCmd& v = b.variants[c.arg & 0x7FFFFFFFu];
        g_warmVars.insert(Var{g_vsKey[v.vsId], g_fsKey[v.fsId], v.layoutMask, v.blendKey});
        ++g_warmCmds;
    };
    for (const vgx::Cmd& c : b.cmds) noteWarm(c);
    for (const vgx::Cmd& c : b.warm) noteWarm(c);
    g_h = 14695981039346656037ull;
    for (const vgx::Cmd& c : b.cmds) {
        if (c.type == vgx::CMD_SHADER || c.type == vgx::CMD_PROGRAM || c.type == vgx::CMD_VARIANT || c.type == vgx::CMD_TEX_UPLOAD ||
            c.type == vgx::CMD_TEX_FREE || c.type == vgx::CMD_BUF_UPLOAD || c.type == vgx::CMD_BUF_FREE)
            continue;
        const uint32_t v[2] = {c.type, c.arg};
        Mix(v, sizeof v);
    }
    for (const vgx::DrawRec& d : b.draws) {
        const uint64_t ks[2] = {g_vsKey.count(d.vsId) ? g_vsKey[d.vsId] : 0xBADull, g_fsKey.count(d.fsId) ? g_fsKey[d.fsId] : 0xBADull};
        Mix(ks, sizeof ks);
        const uint32_t v[14] = {d.prim, d.texMask, d.cover, static_cast<uint32_t>(d.exX), static_cast<uint32_t>(d.exY), static_cast<uint32_t>(d.exW),
                                static_cast<uint32_t>(d.exH), d.layoutMask, d.vbuf, d.vtxOffset, d.vtxCount, d.idxOffset, d.idxCount, d.vtxFirst};
        Mix(v, sizeof v);
        Mix(d.ublock, sizeof d.ublock);
        Mix(&d.rs, sizeof d.rs);
        for (int m = 0; m < 8; ++m) {
            vgx::TexBind t = d.tex[m];
            const uint64_t k = t.texId ? (g_texKey.count(t.texId) ? g_texKey[t.texId] : 0xBADull) : 0;
            t.texId = 0;
            Mix(&t, sizeof t);
            Mix(&k, sizeof k);
        }
        if (d.vtxCount != 15) g_realVars.emplace_back(g_frame, Var{ks[0], ks[1], d.layoutMask, vgx::BlendKey(d.rs)}); // (15: the fixture's meshes)
    }
    Mix(b.vtx.data(), b.vtx.size());
    Mix(b.idx.data(), b.idx.size() * 2);
    Mix(b.uni.data(), b.uni.size() * 4);
    std::printf("frame %u: %016llx  %zu draws %zu vtx %zu idx %zu uni\n", g_frame, static_cast<unsigned long long>(g_h), b.draws.size(), b.vtx.size(),
                b.idx.size(), b.uni.size());
    vgx::backend::Execute(b);
}
} // namespace

extern "C" uint8_t* fe_ram() { return g_ram; }
extern "C" uint32_t fe_alloc(uint32_t bytes) {
    const uint32_t a = g_brk;
    g_brk += (bytes + 31u) & ~31u;
    if (g_brk > kRam) { std::fprintf(stderr, "prewarm test: guest RAM exhausted\n"); std::exit(2); }
    return a;
}
extern "C" void fe_written(uint32_t addr, uint32_t bytes) { vgx::hle::NotifyGuestWrite(g_ram + addr, bytes); }
extern "C" void fe_init() {
    // (the PS Vita player's defaults that change the front end's keys and work, as tests/vgx_fe_work.cpp and vita/vgx-game/entry.cpp set
    // them; the environment overrides them)
    for (const char* kv : {"GXV_FIX_TEVIN=1", "GXV_FS_HALF=1", "GXV_FIX_LODBIAS=1", "GXV_FOGVTX=2", "GXV_RIGIDMERGE=1", "GXV_STATICCULL=1", "GXV_SOFTXFORM=32",
                           "GXV_TEXBAKE=1", "GXV_PALROWS=1", "GXV_LTMERGE=1", "GXV_FIX_DSTA=1", "GXV_FIX_INDPREV=1", "GXV_FIX_LOGICOP=1", "GXV_FIX_TEXOFF=1",
                           "GXV_TEVWRAP=2", "GXV_DSTALPHA_ZERO=1", "GXV_NODISCARD=1", "GXV_TEVIN_CANON=2", "GXV_FS_FINCLAMP=1", "GXV_VSOUT=1",
                           "GXV_TEXVEC2=1", "GXV_DISCARD_BLEND=1", "GXV_IND_VS=1", "GXV_DYNCULL=4", "GXV_FIX_LINES=1"}) {
        const char* eq = std::strchr(kv, '=');
        const std::string k(kv, static_cast<size_t>(eq - kv));
        setenv(k.c_str(), eq + 1, 0);
    }
    vgx::hle::RegisterGuestRam(g_ram, kRam);
    vgx::GuestHooks hooks{};
    hooks.resolve = Resolve;
    vgx::SetGuestHooks(hooks);
    vgx::ResetState();
    vgx::SetFrameSink(&Observe);
    if (std::getenv("PW_DEFER")) vgx::SetShaderWarmupDeferred(true);
    if (std::getenv("PW") && std::atoi(std::getenv("PW")) != 0) g_closedFrames = std::getenv("PW_CLOSED") ? static_cast<unsigned>(std::atoi(std::getenv("PW_CLOSED"))) : 4;
    vgx::g.efbW = 640;
    vgx::g.efbH = 456;
    vgx::QueueSetTarget();
}
extern "C" size_t port_vgx_consume(const uint8_t* p, size_t n) {
    size_t rd = 0, pub = 0;
    while (rd < n) {
        pub = std::min(n, pub + 1 + PieceRn(3000));
        const uint32_t k = vgx::ProcessStream(p + rd, static_cast<uint32_t>(pub - rd), false);
        rd += k;
        if (pub == n && k == 0) break;
    }
    return rd;
}
extern "C" void fe_frame_end(unsigned frame) {
    g_frame = frame;
    vgx::g.dispCopySrc = vgx::EfbRect{0, 0, 640, 456};
    vgx::QueueCopyDisp(true);
    vgx::FrontPresent(false, nullptr);
}
// (a) with the warm-up on (closedFrames > 0); always the counters. Returns the number of failures.
extern "C" int fe_report(unsigned closedFrames) {
    uint64_t pw[7] = {};
    vgx::PrewarmCounters(pw);
    std::fprintf(stderr, "prewarm test: front end: %llu scopes, %llu draws, %llu variants derived, %llu queued, %llu unfolded twins, %llu other-path, %llu unmatched; "
                         "%llu warm variant commands seen, %zu distinct\n",
                 static_cast<unsigned long long>(pw[0]), static_cast<unsigned long long>(pw[1]), static_cast<unsigned long long>(pw[2]),
                 static_cast<unsigned long long>(pw[3]), static_cast<unsigned long long>(pw[4]), static_cast<unsigned long long>(pw[5]),
                 static_cast<unsigned long long>(pw[6]), static_cast<unsigned long long>(g_warmCmds), g_warmVars.size());
    if (closedFrames == 0) return pw[0] != 0 ? 1 : 0; // (off: no scope may have been opened)
    int fail = 0;
    size_t real = 0, missing = 0;
    std::set<Var> realSet;
    for (const auto& fv : g_realVars) {
        if (fv.first <= closedFrames) continue; // (the meshes of the closed frames: not effects, never warmed)
        ++real;
        realSet.insert(fv.second);
    }
    std::set<Var> meshVars; // (the closed frames have no effect emitted: their records are the meshes')
    for (const auto& fv : g_realVars) if (fv.first <= closedFrames) meshVars.insert(fv.second);
    for (const Var& v : realSet) {
        if (g_warmVars.count(v) || meshVars.count(v)) continue;
        ++missing;
        if (missing <= 6) {
            unsigned first = 0, n = 0;
            for (const auto& fv : g_realVars) if (fv.second == v) { if (!first) first = fv.first; ++n; }
            std::fprintf(stderr, "prewarm test: FAIL (a): a real draw's variant vs %016llx fs %016llx layout %03x blend %04x was not warmed (%u records, the first in frame %u)\n",
                         static_cast<unsigned long long>(std::get<0>(v)), static_cast<unsigned long long>(std::get<1>(v)), std::get<2>(v), std::get<3>(v), n, first);
        }
    }
    // (no effect's record may use a shader made after the closed frames: its key would have been new at the real draw. The meshes make keys in
    // play - they draw with what the effects emitted before them left -, the same with the scopes and without.)
    unsigned late = 0;
    for (const auto& fv : g_realVars) {
        if (fv.first <= closedFrames) continue;
        if (g_lateKeys.count(std::get<0>(fv.second)) || g_lateKeys.count(std::get<1>(fv.second))) ++late;
    }
    if (missing) ++fail;
    if (late) { ++fail; std::fprintf(stderr, "prewarm test: FAIL (a): %u effect records use a shader made after the closed frames (a key the scopes did not make)\n", late); }
    if (pw[5] || pw[6]) { ++fail; std::fprintf(stderr, "prewarm test: FAIL: %llu other-path draws, %llu unmatched markers\n", static_cast<unsigned long long>(pw[5]), static_cast<unsigned long long>(pw[6])); }
    std::fprintf(stderr, "prewarm test: (a) %zu effect records after the closed frames, %zu distinct variants, %zu not warmed, %u using a shader made late: %s\n", real,
                 realSet.size(), missing, late, fail ? "FAIL" : "PASS");
    return fail;
}
