// GXV_DRAWREPLAY (vgx_dlcache.cpp "draw replay"): a synthetic, deterministic top-level command stream shaped like the game's
// pictures, through the real front end: a scene of objects drawn in the same order every frame (per object a material list, per shape
// a matrix load, a vertex-format list, array bases and a geometry list), two passes, objects coming and going and swapping places,
// lists moving, being rewritten (same bytes, changed bytes), dynamic geometry, a list's memory reused for a material list and back,
// tiny lists, immediate draws, cut into chunks at random points as the player gets it. Prints a hash of every command buffer: runs
// with GXV_DRAWREPLAY=0 and =1 (and GXV_SPLIT=0 / 1, GXV_FE_PREFETCH) must print the same lines and learn the same shader list.
//   vgx_drawreplay [frames] [seed]      (tools/test_vgx_drawreplay.sh)
//   DRP_REPEAT=<n>: each frame's stream is played n-1 extra times first (into the same frame) and the player's CPU time of those is
//   printed (timing only; DRP_THRASH=<MiB> written between plays, as the game and the back end evict the front end's structures).
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

// ---- the scene: objects drawn in the same order every frame (as the game's), each a material list and one to three shapes
struct List { uint32_t addr = 0, size = 0; };
std::vector<List> g_mats, g_tinyMats;
std::vector<Bytes> g_matBytes;
List g_fmt[3];                 // vertex formats: 0 indexed positions, 1 the same with a per-vertex matrix index, 2 positions and normals
std::vector<List> g_geo[3];    // geometry lists for each format
std::vector<Bytes> g_geoBytes[3];
std::vector<List> g_tinyGeo;   // (format 0)
uint32_t g_posArray[2] = {}, g_nrmArray = 0, g_mtxArray = 0;
constexpr uint32_t kVerts = 96;
List g_dyn;                    // a geometry list written with new bytes every frame (dynamic geometry)
List g_cham;                   // memory that holds a geometry list, then a material list, then a geometry list again ...
Bytes g_chamGeo, g_chamMat;
bool g_chamIsGeo = true;

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
    if (Rn(4) == 0) b.XF(0x100E, {Rnd() & 0x7FFF});                              // channel control
    return b;
}
Bytes VtxFmt(int kind) {
    Bytes b;
    uint32_t vcd = 3u << 9;                    // POS index16
    if (kind == 1) vcd |= 1u;                  // PNMTXIDX direct
    if (kind == 2) vcd |= 2u << 11;            // NRM index8
    b.CP(0x50, vcd);
    b.CP(0x60, 0);
    b.CP(0x70, 1u | (4u << 1) | (kind == 2 ? (4u << 10) : 0u)); // format 0: POS xyz f32 (NRM f32)
    b.CP(0x80, 0);
    b.CP(0x90, 0);
    b.XF(0x1008, {kind == 2 ? 1u << 2 : 0u});    // (as J3D's vertex-format lists: XF 0x1008)
    return b;
}
Bytes Geometry(int kind, uint32_t prims, uint32_t maxStrip) {
    Bytes b;
    for (uint32_t p = 0; p < prims; ++p) {
        const uint32_t n = 3 + Rn(maxStrip);
        b.U8(Rn(6) ? 0x98 : 0x90); // triangle strip / triangles, format 0
        const uint32_t cnt = (b.back() == 0x90) ? (n / 3) * 3 : n;
        b.U16(cnt);
        for (uint32_t i = 0; i < cnt; ++i) {
            if (kind == 1) b.U8(Rn(4) * 3);
            b.U16(Rn(kVerts));
            if (kind == 2) b.U8(Rn(kVerts));
        }
    }
    return b;
}
uint32_t Pad(size_t n) { return static_cast<uint32_t>((n + 31) & ~size_t(31)); }
List NewList(const Bytes& b, uint32_t minSize = 96) {
    List l{0, Pad(b.size())};
    if (l.size < minSize) l.size = minSize;
    l.addr = Alloc(l.size);
    Store(l.addr, b, l.size);
    return l;
}

struct Shape { int kind; int geo; int arr; bool idxMtx; };
struct Object { int mat; std::vector<Shape> shapes; bool visible = true; int extra = -1; };
std::vector<Object> g_objs;

void Build() {
    for (int a = 0; a < 2; ++a) {
        g_posArray[a] = Alloc(kVerts * 12);
        for (uint32_t i = 0; i < kVerts * 3; ++i) Put32(g_ram + g_posArray[a] + i * 4, F2u(Rf(-1, 1)));
        vgx::hle::NotifyGuestWrite(g_ram + g_posArray[a], kVerts * 12);
    }
    g_nrmArray = Alloc(kVerts * 12);
    for (uint32_t i = 0; i < kVerts * 3; ++i) Put32(g_ram + g_nrmArray + i * 4, F2u(Rf(-1, 1)));
    vgx::hle::NotifyGuestWrite(g_ram + g_nrmArray, kVerts * 12);
    g_mtxArray = Alloc(16 * 48);
    for (uint32_t i = 0; i < 16 * 12; ++i) Put32(g_ram + g_mtxArray + i * 4, F2u(Rf(-1, 1)));
    vgx::hle::NotifyGuestWrite(g_ram + g_mtxArray, 16 * 48);
    for (int i = 0; i < 24; ++i) {
        Bytes b = Material();
        List l = NewList(b, 32);
        l.size += 32 * Rn(2);
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
    for (int k = 0; k < 3; ++k) {
        g_fmt[k] = NewList(VtxFmt(k), 32);
        for (int i = 0; i < 30; ++i) {
            // (most are larger than the CPU-transform limit and become static buffers; some stay in the arena)
            const bool small = Rn(4) == 0;
            Bytes gb = small ? Geometry(k, 1 + Rn(3), 6) : Geometry(k, 4 + Rn(20), 10);
            g_geo[k].push_back(NewList(gb));
            g_geoBytes[k].push_back(gb);
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
    g_dyn = NewList(Geometry(0, 12, 10));
    g_dyn.size = 1024;
    Alloc(1024);
    g_chamGeo = Geometry(0, 6, 8);
    g_chamMat = Material();
    g_cham = NewList(g_chamGeo.size() > g_chamMat.size() ? g_chamGeo : g_chamMat);
    Store(g_cham.addr, g_chamGeo, g_cham.size);
    for (int o = 0; o < 70; ++o) {
        Object ob;
        ob.mat = static_cast<int>(Rn(static_cast<uint32_t>(g_mats.size())));
        const int n = 1 + static_cast<int>(Rn(3));
        for (int s = 0; s < n; ++s) {
            Shape sh;
            sh.kind = Rn(5) == 0 ? 1 : (Rn(4) == 0 ? 2 : 0);
            sh.geo = static_cast<int>(Rn(static_cast<uint32_t>(g_geo[sh.kind].size())));
            sh.arr = Rn(6) == 0 ? 1 : 0; // (the second array: another variant of the same list)
            sh.idxMtx = Rn(3) == 0;
            ob.shapes.push_back(sh);
        }
        ob.extra = static_cast<int>(Rn(14));
        g_objs.push_back(ob);
    }
}

// Between frames: lists and arrays written again, mostly with the bytes they hold (J3D's material lists on drawn turns), sometimes changed;
// objects come and go, swap places; a list moves; a list's memory is reused for another kind of list.
void Mutate(unsigned frame) {
    static const bool still = std::getenv("DRP_STILL") != nullptr; // (timing: the scene as it is, only new matrices every frame)
    if (still) return;
    for (size_t m = 0; m < g_mats.size(); ++m) {
        if (Rn(40) == 0) {
            Bytes b = Material();
            if (Pad(b.size()) <= g_mats[m].size) g_matBytes[m] = b;
        }
        if (Rn(2)) Store(g_mats[m].addr, g_matBytes[m], g_mats[m].size);
    }
    if (Rn(30) == 0) { // positions moved
        const uint32_t v = Rn(kVerts);
        const uint32_t a = g_posArray[Rn(2)];
        Put32(g_ram + a + v * 12, F2u(Rf(-1, 1)));
        vgx::hle::NotifyGuestWrite(g_ram + a + v * 12, 12);
    } else if (Rn(3) == 0) {
        vgx::hle::NotifyGuestWrite(g_ram + g_posArray[0], kVerts * 12); // (the same bytes)
    } else if (Rn(5) == 0) {
        vgx::hle::NotifyGuestWrite(g_ram + g_nrmArray + 4000 % (kVerts * 12), 4); // (a write near, not on what a list reads)
    }
    if (Rn(10) == 0) { // a geometry list rebuilt in place
        const int k = static_cast<int>(Rn(3));
        const uint32_t i = Rn(static_cast<uint32_t>(g_geo[k].size()));
        Bytes gb = Geometry(k, 2 + Rn(6), 8);
        if (Pad(gb.size()) <= g_geo[k][i].size) { Store(g_geo[k][i].addr, gb, g_geo[k][i].size); g_geoBytes[k][i] = gb; }
    } else if (Rn(8) == 0) { // the same bytes written again
        const int k = static_cast<int>(Rn(3));
        const uint32_t i = Rn(static_cast<uint32_t>(g_geo[k].size()));
        Store(g_geo[k][i].addr, g_geoBytes[k][i], g_geo[k][i].size);
    }
    if (Rn(25) == 0) { // a list moves to new memory (the old one is left as it is)
        const int k = static_cast<int>(Rn(3));
        const uint32_t i = Rn(static_cast<uint32_t>(g_geo[k].size()));
        if (g_brk + 4096 < kRam) g_geo[k][i] = NewList(g_geoBytes[k][i]);
    }
    {
        Bytes gb = Geometry(0, 4 + Rn(8), 10);
        if (Pad(gb.size()) <= g_dyn.size) Store(g_dyn.addr, gb, g_dyn.size);
    }
    if (frame % 23 == 0) {
        g_chamIsGeo = !g_chamIsGeo;
        Store(g_cham.addr, g_chamIsGeo ? g_chamGeo : g_chamMat, g_cham.size);
    }
    for (Object& o : g_objs) {
        if (Rn(o.visible ? 30 : 6) == 0) o.visible = !o.visible;
        if (Rn(200) == 0) o.mat = static_cast<int>(Rn(static_cast<uint32_t>(g_mats.size())));
    }
    if (Rn(3) == 0) { // two neighbours swap places (depth sorting)
        const size_t i = Rn(static_cast<uint32_t>(g_objs.size() - 1));
        std::swap(g_objs[i], g_objs[i + 1]);
    }
    if (frame > 300 && frame < 340) g_objs[5].visible = false; // (and one is away for a while)
}

void ShapeCmds(Bytes& f, const Shape& sh, bool firstOfObj) {
    if (!sh.idxMtx) {
        std::vector<uint32_t> w;
        for (int i = 0; i < 12; ++i) w.push_back(F2u(Rf(-2, 2)));
        f.XF(0, w);
        if (sh.kind == 2) {
            std::vector<uint32_t> n;
            for (int i = 0; i < 9; ++i) n.push_back(F2u(Rf(-1, 1)));
            f.XF(0x400, n);
        }
    } else {
        f.U8(0x20); f.U16(Rn(16)); f.U16((11u << 12) | 0); // indexed position matrix load (12 words to row 0)
    }
    if (firstOfObj || Rn(2)) f.CP(0x30, 0);
    if (sh.kind == 1) f.XF(0x1018, {0});
    f.Call(g_fmt[sh.kind].addr, g_fmt[sh.kind].size);
    f.CP(0xA0, g_posArray[sh.arr]);
    f.CP(0xB0, 12);
    if (sh.kind == 2) { f.CP(0xA1, g_nrmArray); f.CP(0xB1, 12); }
    const List& gl = g_geo[sh.kind][sh.geo];
    f.Call(gl.addr, gl.size);
}

Bytes Frame() {
    Bytes f;
    f.CP(0xAC, g_mtxArray); // (matrix array for the indexed loads)
    f.CP(0xBC, 48);
    for (int pass = 0; pass < 2; ++pass) { // (two passes over the objects, as a picture with a reflection or an indirect pass has)
        for (const Object& o : g_objs) {
            if (!o.visible || (pass == 1 && (o.mat & 1))) continue;
            const List& m = g_mats[o.mat];
            f.Call(m.addr, m.size);
            for (size_t s = 0; s < o.shapes.size(); ++s) ShapeCmds(f, o.shapes[s], s == 0);
            switch (o.extra) {
            case 0: { const List& t = g_tinyMats[o.mat % g_tinyMats.size()]; f.Call(t.addr, t.size); break; }
            case 1: { const List& t = g_tinyGeo[o.mat % g_tinyGeo.size()]; f.CP(0x50, 3u << 9); f.CP(0x70, 1u | (4u << 1)); f.Call(t.addr, t.size); break; }
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
            case 5: f.Call(0, 32); f.Call(g_posArray[0], 0); break; // (ignored: no address / no size)
            case 6: f.CP(0x50, 3u << 9); f.CP(0x70, 1u | (4u << 1)); f.CP(0xA0, g_posArray[0]); f.Call(g_dyn.addr, g_dyn.size); break;
            case 7: f.CP(0x50, 3u << 9); f.CP(0x70, 1u | (4u << 1)); f.CP(0xA0, g_posArray[0]); f.Call(g_cham.addr, g_cham.size); break;
            case 8: f.Call(o.shapes[0].kind == 0 ? g_geo[0][o.shapes[0].geo].addr : g_geo[0][0].addr, g_geo[0][0].size); break; // (a list called again)
            default: break;
            }
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

namespace vgx { void DrawReplayCounters(uint64_t out[4]) __attribute__((weak)); } // (weak: the test also builds against sources without it, as the reference)

int main(int argc, char** argv) {
    const unsigned frames = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 120;
    if (argc > 2) g_rng ^= static_cast<uint64_t>(std::atoll(argv[2])) * 0x9E3779B97F4A7C15ull;
    vgx::hle::RegisterGuestRam(g_ram, kRam);
    vgx::GuestHooks hooks{};
    hooks.resolve = Resolve;
    vgx::SetGuestHooks(hooks);
    vgx::ResetState();
    vgx::SetFrameSink(&Observe);
    if (const char* list = std::getenv("DRP_LIST")) vgx::SetShaderListFile(list); // (must not exist: compare the files of two runs)
    vgx::g.efbW = 640;
    vgx::g.efbH = 456;
    vgx::QueueSetTarget();
    Build();
    uint64_t chunks = 0, bytes = 0;
    const long thrashMb = std::getenv("DRP_THRASH") ? std::atol(std::getenv("DRP_THRASH")) : 0;
    const long repeat = std::getenv("DRP_REPEAT") ? std::max(1L, std::atol(std::getenv("DRP_REPEAT"))) : 1;
    std::vector<uint8_t> thrash(static_cast<size_t>(thrashMb) << 20);
    double feMs = 0;
    for (g_frame = 1; g_frame <= frames; ++g_frame) {
        const uint64_t rngAtFrame = g_rng;
        Bytes f = Frame();
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
            vgx::g.dispCopySrc = vgx::EfbRect{0, 0, 640, 456};
            vgx::QueueCopyDisp(true);
            vgx::FrontPresent(false, nullptr); // (a frame of its own, so that the next play is the same picture again)
        }
        (void)rngAtFrame;
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
        Mutate(g_frame);
    }
    uint64_t rp[4] = {};
    if (&vgx::DrawReplayCounters) vgx::DrawReplayCounters(rp);
    std::fprintf(stderr, "drawreplay fixture: %u frames, %llu stream bytes in %llu pieces; replay hits %llu, misses %llu, realigned %llu, recorded %llu\n",
                 frames, static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(chunks), static_cast<unsigned long long>(rp[0]),
                 static_cast<unsigned long long>(rp[1]), static_cast<unsigned long long>(rp[2]), static_cast<unsigned long long>(rp[3]));
    if (repeat > 1) std::fprintf(stderr, "drawreplay fixture: extra plays %.3f ms of player CPU a frame (DRP_THRASH=%ld MiB)\n", feMs / frames / (repeat - 1), thrashMb);
    return 0;
}
