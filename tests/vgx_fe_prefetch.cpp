// GXV_FE_PREFETCH / GXV_VARFRONT (vgx_state.cpp "look-ahead", vgx_draw.cpp NoteVariant): a synthetic, deterministic top-level command
// stream shaped like the game's (per shape: CALL_DL of a material list, a matrix load, CALL_DL of a vertex-format list, array bases,
// CALL_DL of a geometry list; tiny lists, immediate draws, padding, no-ops, indexed matrix loads), cut into chunks at random points
// as the player gets it, through the real front end. Lists and arrays live in registered guest RAM and are rewritten between frames
// (same bytes, changed bytes) so that the state-list and geometry caches take all their branches. Prints a hash of every command
// buffer; runs with GXV_FE_PREFETCH=0 and 1..4 (and GXV_VARFRONT=0 / 1, GXV_SPLIT=0 / 1) must print the same lines and learn the same
// shader warm-up list. Built with AddressSanitizer it checks that the look-ahead reads nothing outside the stream.
//   vgx_fe_prefetch [frames] [seed]      (tools/test_vgx_fe_prefetch.sh)
#include "vgx_state.h"
#include "vgx_fesplit.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

extern "C" int64_t port_host_time_ns() { static std::atomic<int64_t> n{1}; return n.fetch_add(1000); }
extern "C" int64_t port_thread_cpu_us() { static std::atomic<int64_t> n{1}; return n.fetch_add(1); }
extern "C" void port_log_summary(const char*, ...) {}
extern "C" void port_log(const char*, ...) {}

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

// ---- guest RAM: the console addresses are offsets into it (address 0 is never used)
constexpr uint32_t kRam = 4u << 20;
alignas(4096) uint8_t g_ram[kRam];
uint32_t g_brk = 0x1000;
uint32_t Alloc(uint32_t bytes) {
    const uint32_t a = g_brk;
    g_brk += (bytes + 31u) & ~31u;
    if (g_brk > kRam) { std::fprintf(stderr, "fixture: guest RAM exhausted\n"); std::exit(2); }
    return a;
}
const uint8_t* Resolve(uint32_t address, uint32_t* avail) {
    *avail = 0;
    if (address == 0 || address >= kRam) return nullptr;
    *avail = kRam - address;
    return g_ram + address;
}

void Put32(uint8_t* p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i)); }
void Put16(uint8_t* p, uint32_t v) { p[0] = static_cast<uint8_t>(v >> 8); p[1] = static_cast<uint8_t>(v); }
struct Bytes : std::vector<uint8_t> {
    void U8(uint32_t v) { push_back(static_cast<uint8_t>(v)); }
    void U16(uint32_t v) { const size_t a = size(); resize(a + 2); Put16(data() + a, v); }
    void U32(uint32_t v) { const size_t a = size(); resize(a + 4); Put32(data() + a, v); }
    void BP(uint32_t reg, uint32_t value) { U8(0x61); U32(reg << 24 | (value & 0xFFFFFF)); }
    void CP(uint32_t reg, uint32_t value) { U8(0x08); U8(reg); U32(value); }
    void XF(uint32_t addr, const std::vector<uint32_t>& w) { U8(0x10); U16(static_cast<uint32_t>(w.size() - 1)); U16(addr); for (uint32_t x : w) U32(x); }
    void Call(uint32_t addr, uint32_t size) { U8(0x40); U32(addr); U32(size); }
};
// A list in guest RAM: `b` padded with zeros to `bytes`, the write notified.
void Store(uint32_t addr, const Bytes& b, uint32_t bytes) {
    if (b.size() > bytes) { std::fprintf(stderr, "fixture: list does not fit\n"); std::exit(2); }
    std::memset(g_ram + addr, 0, bytes);
    std::memcpy(g_ram + addr, b.data(), b.size());
    vgx::hle::NotifyGuestWrite(g_ram + addr, bytes);
}

// ---- the lists
struct List { uint32_t addr = 0, size = 0; };
std::vector<List> g_mats, g_tinyMats;
std::vector<Bytes> g_matBytes;
List g_shape[2];               // vertex formats: 0 indexed positions, 1 the same with a per-vertex matrix index
std::vector<List> g_geo[2];    // geometry lists for each
std::vector<List> g_tinyGeo;   // (shape 0)
uint32_t g_posArray = 0, g_mtxArray = 0;
constexpr uint32_t kVerts = 64;

Bytes Material() {
    Bytes b;
    const uint32_t stages = 1 + Rn(3);
    b.BP(0x00, (1u << 4) | ((stages - 1) << 10)); // genMode: no texgens, one colour channel
    for (uint32_t s = 0; s < stages; ++s) {
        b.BP(0xC0 + 2 * s, Rnd() & 0xFFFFFF);                                    // TEV colour env
        b.BP(0xC1 + 2 * s, (Rnd() & 0xFFFFF0) | 0);                              // TEV alpha env
    }
    b.BP(0x28, (Rnd() & 0xFFFFFF & ~0x40040u) | 0x7 | (0x7u << 12));            // TEV order, no texture
    for (uint32_t r = 0xE0; r <= 0xE7; ++r)
        if (Rn(2)) b.BP(r, (Rnd() & 0x7FF7FF) | (Rn(2) ? 0x800000u : 0u));       // colour / konst registers
    b.BP(0xF3, Rnd() & 0xFFFFFF);                                                // alpha compare
    if (Rn(2)) b.BP(0x41, 0x0004A0 | Rn(2));                                    // blend mode
    if (Rn(2)) b.BP(0x40, 0x17 | (Rn(2) << 4));                                 // z mode
    b.XF(0x100C, {Rnd()});                                                       // material colour
    if (Rn(3) == 0) b.XF(0x1009, {1});                                           // colour channels
    return b;
}
Bytes ShapeFmt(int kind) {
    Bytes b;
    b.CP(0x50, (kind ? 1u : 0u) | (3u << 9)); // (PNMTXIDX direct) POS index16
    b.CP(0x60, 0);
    b.CP(0x70, 1u | (4u << 1));               // format 0: POS xyz f32
    b.CP(0x80, 0);
    b.CP(0x90, 0);
    b.XF(0x1008, {0});                         // (as J3D's vertex-format lists: XF 0x1008)
    return b;
}
Bytes Geometry(int kind, uint32_t prims) {
    Bytes b;
    for (uint32_t p = 0; p < prims; ++p) {
        const uint32_t n = 3 + Rn(8);
        b.U8(0x98); // triangle strip, format 0
        b.U16(n);
        for (uint32_t i = 0; i < n; ++i) {
            if (kind) b.U8(Rn(4) * 3);
            b.U16(Rn(kVerts));
        }
    }
    return b;
}
uint32_t Pad(size_t n) { return static_cast<uint32_t>((n + 31) & ~size_t(31)); }

void Build() {
    g_posArray = Alloc(kVerts * 12);
    for (uint32_t i = 0; i < kVerts * 3; ++i) Put32(g_ram + g_posArray + i * 4, F2u(Rf(-1, 1)));
    vgx::hle::NotifyGuestWrite(g_ram + g_posArray, kVerts * 12);
    g_mtxArray = Alloc(16 * 48);
    for (uint32_t i = 0; i < 16 * 12; ++i) Put32(g_ram + g_mtxArray + i * 4, F2u(Rf(-1, 1)));
    vgx::hle::NotifyGuestWrite(g_ram + g_mtxArray, 16 * 48);
    for (int i = 0; i < 48; ++i) {
        Bytes b = Material();
        List l;
        l.size = Pad(b.size()) + 32 * Rn(2);
        l.addr = Alloc(l.size);
        Store(l.addr, b, l.size);
        g_mats.push_back(l);
        g_matBytes.push_back(b);
    }
    for (int i = 0; i < 6; ++i) {
        Bytes b;
        b.BP(0xE2 + 2 * Rn(2), Rnd() & 0x7FF7FF);
        b.BP(0x41, 0x0004A0 | Rn(2));
        List l{Alloc(32), 32};
        Store(l.addr, b, 32);
        g_tinyMats.push_back(l);
    }
    for (int k = 0; k < 2; ++k) {
        Bytes b = ShapeFmt(k);
        g_shape[k] = List{Alloc(Pad(b.size())), Pad(b.size())};
        Store(g_shape[k].addr, b, g_shape[k].size);
        for (int i = 0; i < 40; ++i) {
            Bytes gb = Geometry(k, 2 + Rn(12));
            List l{0, Pad(gb.size())};
            if (l.size <= 64) l.size = 96;
            l.addr = Alloc(l.size);
            Store(l.addr, gb, l.size);
            g_geo[k].push_back(l);
        }
    }
    for (int i = 0; i < 8; ++i) {
        Bytes gb;
        gb.U8(0x90); // triangles, format 0
        gb.U16(3);
        for (int v = 0; v < 3; ++v) gb.U16(Rn(kVerts));
        List l{Alloc(32), 32};
        Store(l.addr, gb, 32);
        g_tinyGeo.push_back(l);
    }
}

// Between frames: lists and arrays written again, mostly with the bytes they hold (J3D's material lists on drawn turns), sometimes changed.
void Mutate() {
    for (int i = 0; i < 6; ++i) {
        const uint32_t m = Rn(static_cast<uint32_t>(g_mats.size()));
        if (Rn(5) == 0) {
            Bytes b = Material();
            if (b.size() <= g_mats[m].size) g_matBytes[m] = b;
        }
        Store(g_mats[m].addr, g_matBytes[m], g_mats[m].size);
    }
    if (Rn(4) == 0) { // positions moved
        const uint32_t v = Rn(kVerts);
        Put32(g_ram + g_posArray + v * 12, F2u(Rf(-1, 1)));
        vgx::hle::NotifyGuestWrite(g_ram + g_posArray + v * 12, 12);
    } else if (Rn(3) == 0) {
        vgx::hle::NotifyGuestWrite(g_ram + g_posArray, kVerts * 12); // (the same bytes)
    }
    if (Rn(6) == 0) { // a geometry list rebuilt in place
        const int k = static_cast<int>(Rn(2));
        List& l = g_geo[k][Rn(static_cast<uint32_t>(g_geo[k].size()))];
        Bytes gb = Geometry(k, 1 + Rn(4));
        if (Pad(gb.size()) <= l.size) Store(l.addr, gb, l.size);
    }
}

Bytes Frame() {
    Bytes f;
    f.CP(0xAC, g_mtxArray); // (matrix array for the indexed loads)
    f.CP(0xBC, 48);
    const uint32_t shapes = 30 + Rn(120);
    for (uint32_t s = 0; s < shapes; ++s) {
        const int k = Rn(4) == 0 ? 1 : 0;
        if (Rn(5)) { const List& m = g_mats[Rn(static_cast<uint32_t>(g_mats.size()))]; f.Call(m.addr, m.size); }
        if (Rn(3)) {
            std::vector<uint32_t> w;
            for (int i = 0; i < 12; ++i) w.push_back(F2u(Rf(-2, 2)));
            f.XF(0, w);
        } else {
            f.U8(0x20); f.U16(Rn(16)); f.U16((11u << 12) | 0); // indexed position matrix load (12 words to row 0)
        }
        f.CP(0x30, 0);
        if (k) f.XF(0x1018, {0});
        f.Call(g_shape[k].addr, g_shape[k].size);
        f.CP(0xA0, g_posArray);
        f.CP(0xB0, 12);
        const List& gl = g_geo[k][Rn(static_cast<uint32_t>(g_geo[k].size()))];
        f.Call(gl.addr, gl.size);
        switch (Rn(12)) {
        case 0: { const List& t = g_tinyMats[Rn(static_cast<uint32_t>(g_tinyMats.size()))]; f.Call(t.addr, t.size); break; }
        case 1: if (!k) { const List& t = g_tinyGeo[Rn(static_cast<uint32_t>(g_tinyGeo.size()))]; f.Call(t.addr, t.size); } break;
        case 2: f.U8(0x44); f.U8(0x48); break;
        case 3: for (uint32_t z = Rn(9); z; --z) f.U8(0); break;
        case 4: { // an immediate draw: direct positions in format 1
            f.CP(0x50, 1u << 9);
            f.CP(0x71, 1u | (4u << 1));
            const uint32_t n = 3 * (1 + Rn(3));
            f.U8(0x91);
            f.U16(n);
            for (uint32_t i = 0; i < n * 3; ++i) f.U32(F2u(Rf(-1, 1)));
            break;
        }
        case 5: f.Call(0, 32); f.Call(g_posArray, 0); break; // (ignored: no address / no size)
        default: break;
        }
    }
    return f;
}

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
        Mix(d.ublock, sizeof d.ublock);
        Mix(&d.rs, sizeof d.rs);
        Mix(d.tex, sizeof d.tex);
    }
    Mix(b.vtx.data(), b.vtx.size());
    Mix(b.idx.data(), b.idx.size() * 2);
    Mix(b.uni.data(), b.uni.size() * 4);
    for (const vgx::BufUpload& u : b.bufUploads) { Mix(u.vtx.data(), u.vtx.size()); Mix(u.idx.data(), u.idx.size() * 2); }
    std::printf("frame %u: %016llx  %zu cmds %zu draws %zu vtx %zu idx %zu uni %zu uploads\n", g_frame, static_cast<unsigned long long>(g_h), b.cmds.size(),
                b.draws.size(), b.vtx.size(), b.idx.size(), b.uni.size(), b.bufUploads.size());
    vgx::backend::Execute(b);
}
} // namespace

int main(int argc, char** argv) {
    const unsigned frames = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 120;
    if (argc > 2) g_rng ^= static_cast<uint64_t>(std::atoll(argv[2])) * 0x9E3779B97F4A7C15ull;
    vgx::hle::RegisterGuestRam(g_ram, kRam);
    vgx::GuestHooks hooks{};
    hooks.resolve = Resolve;
    vgx::SetGuestHooks(hooks);
    vgx::ResetState();
    vgx::SetFrameSink(&Observe);
    if (const char* list = std::getenv("FEPF_LIST")) vgx::SetShaderListFile(list); // (must not exist: compare the files of two runs)
    vgx::g.efbW = 640;
    vgx::g.efbH = 456;
    vgx::QueueSetTarget();
    Build();
    uint64_t chunks = 0, bytes = 0;
    // FEPF_THRASH=<MiB> (timing only): that much memory is written between frames, as the game's and the back end's work push the front
    // end's structures out of the caches on the console; FEPF_REPEAT=<n>: each frame's stream is played n times (the time is the front end's).
    const long thrashMb = std::getenv("FEPF_THRASH") ? std::atol(std::getenv("FEPF_THRASH")) : 0;
    const long repeat = std::getenv("FEPF_REPEAT") ? std::max(1L, std::atol(std::getenv("FEPF_REPEAT"))) : 1;
    std::vector<uint8_t> thrash(static_cast<size_t>(thrashMb) << 20);
    double feMs = 0;
    for (g_frame = 1; g_frame <= frames; ++g_frame) {
        const Bytes f = Frame();
        for (long rep = 1; rep < repeat; ++rep) { // (extra plays of the same stream, into the same frame: timing only)
            for (size_t i = 0; i < thrash.size(); i += 64) thrash[i] = static_cast<uint8_t>(i + rep);
            timespec t0{}, t1{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
            for (size_t rd = 0; rd < f.size();) {
                const uint32_t n = vgx::ProcessStream(f.data() + rd, static_cast<uint32_t>(f.size() - rd), false);
                if (!n) break;
                rd += n;
            }
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t1);
            feMs += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        }
        for (size_t i = 0; i < thrash.size(); i += 64) thrash[i] = static_cast<uint8_t>(i);
        // (as the player gets it: published in pieces; a command the end of a piece cuts is presented again with the rest)
        size_t rd = 0, pub = 0;
        while (rd < f.size()) {
            pub = std::min(f.size(), pub + 1 + Rn(Rn(4) ? 4096 : 64));
            if (pub < rd) pub = rd;
            rd += vgx::ProcessStream(f.data() + rd, static_cast<uint32_t>(pub - rd), false);
            ++chunks;
            if (pub == f.size() && rd < f.size()) {
                const uint32_t n = vgx::ProcessStream(f.data() + rd, static_cast<uint32_t>(f.size() - rd), false);
                if (!n) { std::fprintf(stderr, "fixture: stream stuck at %zu of %zu\n", rd, f.size()); return 2; }
                rd += n;
            }
        }
        bytes += f.size();
        vgx::g.dispCopySrc = vgx::EfbRect{0, 0, 640, 456};
        vgx::QueueCopyDisp(true);
        vgx::FrontPresent(false, nullptr);
        Mutate();
    }
    std::fprintf(stderr, "fe-prefetch fixture: %u frames, %llu stream bytes in %llu pieces%s\n", frames, static_cast<unsigned long long>(bytes),
                 static_cast<unsigned long long>(chunks), repeat > 1 ? "" : "");
    if (repeat > 1) std::fprintf(stderr, "fe-prefetch fixture: extra plays %.2f ms of player CPU a frame (FEPF_THRASH=%ld MiB)\n", feMs / frames / (repeat - 1), thrashMb);
    return 0;
}
