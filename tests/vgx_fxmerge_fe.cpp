// GXV_FXMERGE end to end (tools/test_vgx_fxmerge.sh): the front-end half. The game half (tests/vgx_fxmerge.cpp) drives the real GX
// library units and the real gx_fifo.cpp; what the FIFO hands over (port_vgx_consume) is parsed here by the real front end, in pieces
// as the player gets it, and every picture's command buffer is hashed as tests/vgx_fe_work.cpp hashes it. Runs with the gathering on and
// off must print the same lines.
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
#include <vector>

extern "C" int64_t port_host_time_ns() { static std::atomic<int64_t> n{1}; return n.fetch_add(1000); }
extern "C" int64_t port_thread_cpu_us() { static std::atomic<int64_t> n{1}; return n.fetch_add(1); }
extern "C" void port_log_summary(const char*, ...) {}
extern "C" void port_log(const char* fmt, ...) { // (stderr: the gathering's "a GX write it does not take" line is looked for)
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
void FxMergeCounters(uint64_t out[6]);
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
void Observe(vgx::CmdBuf& b) {
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
    }
    Mix(b.vtx.data(), b.vtx.size());
    Mix(b.idx.data(), b.idx.size() * 2);
    Mix(b.uni.data(), b.uni.size() * 4);
    for (const vgx::TexUpload& u : b.uploads) for (const auto& l : u.levels) Mix(l.data(), l.size());
    std::printf("frame %u: %016llx  %zu cmds %zu draws %zu vtx %zu idx %zu uni\n", g_frame, static_cast<unsigned long long>(g_h), b.cmds.size(),
                b.draws.size(), b.vtx.size(), b.idx.size(), b.uni.size());
    vgx::backend::Execute(b);
}
} // namespace

// (the game half's view of the front end)
extern "C" uint8_t* fe_ram() { return g_ram; }
extern "C" uint32_t fe_alloc(uint32_t bytes) {
    const uint32_t a = g_brk;
    g_brk += (bytes + 31u) & ~31u;
    if (g_brk > kRam) { std::fprintf(stderr, "fxmerge test: guest RAM exhausted\n"); std::exit(2); }
    return a;
}
extern "C" void fe_written(uint32_t addr, uint32_t bytes) { vgx::hle::NotifyGuestWrite(g_ram + addr, bytes); }
extern "C" void fe_init() {
    // (the PS Vita player's defaults that change the front end's work, as tests/vgx_fe_work.cpp sets them; the environment overrides them)
    for (const char* kv : {"GXV_FIX_TEVIN=1", "GXV_FS_HALF=1", "GXV_FIX_LODBIAS=1", "GXV_FOGVTX=2", "GXV_RIGIDMERGE=1", "GXV_STATICCULL=1", "GXV_SOFTXFORM=32",
                           "GXV_TEXBAKE=1", "GXV_PALROWS=1", "GXV_LTMERGE=1", "GXV_FIX_DSTA=1", "GXV_FIX_INDPREV=1", "GXV_FIX_LOGICOP=1", "GXV_FIX_TEXOFF=1",
                           "GXV_TEVWRAP=2", "GXV_DSTALPHA_ZERO=1", "GXV_NODISCARD=1"}) {
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
    vgx::g.efbW = 640;
    vgx::g.efbH = 456;
    vgx::QueueSetTarget();
}
// What the FIFO's flush hands over: parsed in pieces of random size (a cut command is left for the next piece, as the player does);
// returns what was consumed (all but an incomplete command at the end).
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
extern "C" void fe_counters(unsigned long long out[6]) {
    uint64_t fx[6] = {};
    vgx::FxMergeCounters(fx);
    for (int i = 0; i < 6; ++i) out[i] = fx[i];
}
