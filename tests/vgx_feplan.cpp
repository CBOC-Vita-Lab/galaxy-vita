// Mixed display lists' plans (GXV_FEPLAN, vgx_state.cpp "mixed display lists: exact plans", feplan31) against the plain parser: random
// lists that hold state commands AND draws (and CALL_DLs inside the list, NOPs, masked BP writes, indexed matrix loads, XF registers and
// matrix rows, CP matrix indices and array bases, now and then a vertex-descriptor write before a draw, an unknown opcode, a draw that
// runs past the list's end, a command cut by the end), in guest memory, called through CallList again and again. Before a call the
// top-level stream sets the list's vertex format, or now and then another one (a draw of another size than the plan was made with).
// Lists are written again with the same bytes, with one value changed, or written next to. Prints a hash of the whole register state
// after every call, and of every command buffer the front end hands over (the draws: records, vertices, indices, uniforms):
// GXV_FEPLAN=0 (every call parsed), 1 and 2 must print the same lines. tools/test_vgx_feplan.sh.
//   vgx_feplan [calls] [seed]
#include "vgx_hle.h"
#include "vgx_state.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
} // namespace vgx
extern "C" int64_t port_host_time_ns() {
    static int64_t n = 1; // (a counter: runs repeat exactly)
    return n += 1000;
}
extern "C" void port_log_summary(const char* fmt, ...) { // (FEPLAN_LOG=1: the census lines on stderr)
    static const bool on = std::getenv("FEPLAN_LOG") != nullptr;
    if (!on) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

namespace {
uint64_t g_rng = 0x9E3779B97F4A7C15ull;
uint32_t Rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return static_cast<uint32_t>(g_rng >> 11);
}
uint32_t Rn(uint32_t n) { return Rnd() % n; }
float Rf(float a, float b) { return a + (b - a) * static_cast<float>(Rn(10001)) / 10000.0f; }
uint32_t F2u(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

constexpr uint32_t kRam = 2u << 20;
alignas(4096) uint8_t g_ram[kRam];
const uint8_t* Resolve(uint32_t address, uint32_t* avail) {
    *avail = 0;
    address &= 0x3FFFFFFFu;
    if (address == 0 || address >= kRam) return nullptr;
    *avail = kRam - address;
    return g_ram + address;
}
// guest memory: lists from 0x1000 (2 KiB each), the arrays from 0x100000
constexpr uint32_t kListBase = 0x1000, kListRoom = 2048, kLists = 48;
constexpr uint32_t kPos = 0x100000, kClr = 0x140000, kMtx = 0x160000, kNbr = 0x180000;
constexpr uint32_t kVerts = 1024;

void Put32(uint8_t* p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i)); }
void PutLE(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
struct Bytes : std::vector<uint8_t> {
    void U8(uint32_t v) { push_back(static_cast<uint8_t>(v)); }
    void U16(uint32_t v) { U8(v >> 8); U8(v); }
    void U32(uint32_t v) { U16(v >> 16); U16(v & 0xFFFF); }
    void BP(uint32_t reg, uint32_t value) { U8(0x61); U32(reg << 24 | (value & 0xFFFFFF)); }
    void CP(uint32_t reg, uint32_t value) { U8(0x08); U8(reg); U32(value); }
    void XF(uint32_t addr, const std::vector<uint32_t>& w) { U8(0x10); U16(static_cast<uint32_t>(w.size() - 1)); U16(addr); for (uint32_t x : w) U32(x); }
};

// Vertex configurations (format 0..2 each): the descriptor and the format words, and the bytes a vertex takes in the stream.
struct Cfg { uint32_t vcdLo, vcdHi, vat[3]; uint32_t size; bool direct; };
const Cfg kCfg[4] = {
    {3u << 9 | 3u << 11, 0, {1u | (4u << 1) | (4u << 10), 0, 0}, 4, false},                         // POS, NRM index16
    {1u | 3u << 9 | 2u << 13, 0, {1u | (4u << 1) | (1u << 13) | (5u << 14), 0, 0}, 4, false},       // PNMTXIDX, POS index16, CLR0 index8
    {1u << 9 | 1u << 13, 0, {1u | (4u << 1) | (1u << 13) | (5u << 14), 0, 0}, 16, true},           // POS direct f32 XYZ, CLR0 direct RGBA8
    {3u << 9 | 3u << 11, 3u, {1u | (4u << 1) | (4u << 10) | (1u << 21) | (4u << 22), 0, 0}, 6, false}, // POS, NRM, TEX0 index16
};
void Vertex(Bytes& b, int cfg) {
    switch (cfg) {
    case 0: b.U16(Rn(kVerts)); b.U16(Rn(kVerts)); break;
    case 1: b.U8(Rn(10) * 3); b.U16(Rn(kVerts)); b.U8(Rn(256)); break;
    case 2: for (int i = 0; i < 3; ++i) b.U32(F2u(Rf(-2, 2))); b.U32(Rnd() | 0xFFu); break;
    default: b.U16(Rn(kVerts)); b.U16(Rn(kVerts)); b.U16(Rn(kVerts)); break;
    }
}
void SetCfg(Bytes& f, int cfg) {
    f.CP(0x50, kCfg[cfg].vcdLo);
    f.CP(0x60, kCfg[cfg].vcdHi);
    f.CP(0x70, kCfg[cfg].vat[0]);
    f.CP(0x80, 0);
    f.CP(0x90, 0);
}

struct List { uint32_t addr = 0, size = 0; int cfg = 0; std::vector<uint32_t> values; };
List g_list[kLists];
std::vector<uint32_t> g_padTo;
uint32_t g_refusedKinds[8] = {};

void MakeList(unsigned l) {
    static const uint8_t kBp[] = {0xC0, 0xC1, 0xC2, 0xC3, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0x28, 0x29, 0x40, 0x41, 0x42, 0xF3, 0xF6, 0xF7, 0x10, 0x11, 0x25, 0x26};
    List& L = g_list[l];
    L.addr = kListBase + l * kListRoom;
    L.cfg = static_cast<int>(Rn(4));
    L.values.clear();
    Bytes b;
    const uint32_t segs = 1 + Rn(4);
    const bool fmtBefore = Rn(10) == 0, callIn = Rn(5) == 0, unknown = Rn(14) == 0, past = Rn(10) == 0, cutEnd = Rn(10) == 0, trail = Rn(3) == 0;
    auto value = [&] { L.values.push_back(static_cast<uint32_t>(b.size())); };
    if (fmtBefore) { b.CP(0x50, kCfg[L.cfg].vcdLo); ++g_refusedKinds[0]; }
    auto state = [&](uint32_t n) {
        for (uint32_t c = 0; c < n; ++c) {
            const uint32_t r = Rn(12);
            if (r < 5) {
                if (Rn(10) == 0) b.BP(0xFE, Rnd() & 0xFFFFFF);
                b.U8(0x61); value(); b.U32(static_cast<uint32_t>(kBp[Rn(sizeof kBp)]) << 24 | (Rnd() & 0xFFFFFF));
            } else if (r == 5) {
                b.U8(0x08); b.U8(Rn(2) ? 0x30 : 0x40); value(); b.U32(Rn(2) ? 0u : Rnd());
            } else if (r == 6) {
                b.CP(0xA0, kPos); // (the array base, with the value it holds)
            } else if (r < 9) {
                const uint32_t addr = 0x1009u + Rn(10);
                b.U8(0x10); b.U16(0); b.U16(addr); value(); b.U32(addr == 0x1009u ? Rn(3) : Rnd());
            } else if (r == 9) {
                std::vector<uint32_t> w;
                for (int k = 0; k < 12; ++k) w.push_back(F2u(Rf(-3, 3)));
                b.XF(Rn(20) * 12, w);
            } else if (r == 10) {
                b.U8(0x20); b.U16(Rn(16)); b.U16((11u << 12) | (Rn(10) * 12)); // (a position matrix by its index)
            } else {
                b.U8(Rn(2) ? 0x44 : 0x48);
            }
        }
    };
    for (uint32_t s = 0; s < segs; ++s) {
        state(Rn(10));
        if (callIn && s == segs / 2) { b.U8(0x40); b.U32(kListBase + Rn(kLists) * kListRoom); b.U32(32); }
        const uint32_t n = 1 + Rn(12);
        const bool last = s + 1 == segs;
        b.U8((Rn(2) ? 0x98 : 0x90) | 0); // (a strip or triangles, format 0)
        if (last && past) {
            b.U16(n + 20); // (runs past the end: drawn up to the end, then the parse stops)
            for (uint32_t i = 0; i < n; ++i) Vertex(b, L.cfg);
            L.size = static_cast<uint32_t>(b.size());
            break;
        }
        b.U16(n);
        for (uint32_t i = 0; i < n; ++i) Vertex(b, L.cfg);
        if (Rn(6) == 0) { b.U8(0x98); b.U16(0); } // (a draw of no vertices)
    }
    if (!(past)) {
        if (trail) state(1 + Rn(4));
        if (unknown) b.U8(0x01 + Rn(3)); // (an opcode the parser does not know: it stops there)
        if (cutEnd) {
            b.U8(0x10); b.U16(2); b.U16(0x1009); b.U32(1); // (an XF write of 3 words with only one there: cut)
            L.size = static_cast<uint32_t>(b.size());
        } else {
            while (b.size() % 32) b.U8(0);
            L.size = static_cast<uint32_t>(b.size());
        }
    }
    if (L.size > kListRoom) { std::fprintf(stderr, "list too big\n"); std::exit(2); }
    std::memset(g_ram + L.addr, 0, kListRoom);
    std::memcpy(g_ram + L.addr, b.data(), L.size);
    vgx::hle::NotifyGuestWrite(g_ram + L.addr, kListRoom);
}

uint64_t StateHash() {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&vgx::g);
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < sizeof(vgx::State); ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
uint64_t g_h = 0;
uint64_t g_draws = 0;
void Mix(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) g_h = (g_h ^ b[i]) * 1099511628211ull;
}
void Sink(vgx::CmdBuf& b) {
    g_h = 14695981039346656037ull;
    for (const vgx::Cmd& c : b.cmds) { const uint32_t v[2] = {c.type, c.arg}; Mix(v, sizeof v); }
    for (const vgx::DrawRec& d : b.draws) {
        const uint32_t v[12] = {d.vsId, d.fsId, d.prim, d.texMask, d.cover, d.layoutMask, d.vbuf, d.vtxOffset, d.vtxCount, d.idxOffset, d.idxCount, d.vtxFirst};
        Mix(v, sizeof v);
        Mix(d.ublock, sizeof d.ublock);
        Mix(&d.rs, sizeof d.rs);
        ++g_draws;
    }
    Mix(b.vtx.data(), b.vtx.size());
    Mix(b.idx.data(), b.idx.size() * 2);
    Mix(b.uni.data(), b.uni.size() * 4);
    std::printf("buffer %016llx (%zu draws)\n", static_cast<unsigned long long>(g_h), b.draws.size());
    vgx::backend::Execute(b);
}
void Top(const Bytes& f) {
    uint32_t rd = 0;
    while (rd < f.size()) {
        const uint32_t n = vgx::ProcessStream(f.data() + rd, static_cast<uint32_t>(f.size() - rd), false);
        if (!n) break;
        rd += n;
    }
}
} // namespace

int main(int argc, char** argv) {
    const unsigned calls = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 6000;
    if (argc > 2) g_rng += static_cast<uint64_t>(std::atoll(argv[2])) * 0x2545F4914F6CDD1Dull; // (never 0: xorshift would stay there)
    vgx::hle::RegisterGuestRam(g_ram, kRam);
    vgx::GuestHooks hooks{};
    hooks.resolve = Resolve;
    vgx::SetGuestHooks(hooks);
    vgx::ResetState();
    vgx::SetFrameSink(&Sink);
    vgx::g.efbW = 640;
    vgx::g.efbH = 456;
    vgx::QueueSetTarget();
    for (uint32_t i = 0; i < kVerts * 3; ++i) PutLE(g_ram + kPos + i * 4, F2u(Rf(-1, 1)));
    for (uint32_t i = 0; i < kVerts * 3; ++i) PutLE(g_ram + kNbr + i * 4, F2u(Rf(-1, 1)));
    for (uint32_t i = 0; i < kVerts; ++i) PutLE(g_ram + kClr + i * 4, Rnd());
    for (uint32_t i = 0; i < 16 * 12; ++i) PutLE(g_ram + kMtx + i * 4, F2u(Rf(-2, 2)));
    vgx::hle::NotifyGuestWrite(g_ram + kPos, kRam - kPos);
    for (unsigned l = 0; l < kLists; ++l) MakeList(l);
    {
        Bytes f;
        f.CP(0xA0, kPos); f.CP(0xB0, 12);
        f.CP(0xA1, kNbr); f.CP(0xB1, 12);
        f.CP(0xA2, kClr); f.CP(0xB2, 4);
        f.CP(0xA4, kPos); f.CP(0xB4, 8);
        f.CP(0xAC, kMtx); f.CP(0xBC, 48);
        f.XF(0x1020, {F2u(1.2f), 0, F2u(1.6f), 0, F2u(-1.0f), F2u(-0.2f), 0});
        f.XF(0x1008, {1u | (1u << 2) | (1u << 4)});
        Top(f);
    }
    unsigned rewrites = 0, changes = 0, neighbours = 0, otherCfg = 0, remade = 0;
    for (unsigned call = 1; call <= calls; ++call) {
        const unsigned l = Rn(kLists);
        List& L = g_list[l];
        uint8_t* const list = g_ram + L.addr;
        const uint32_t r = Rn(100);
        if (r < 15) {
            // written again with what it holds
            uint8_t copy[kListRoom];
            std::memcpy(copy, list, L.size);
            std::memcpy(list, copy, L.size);
            vgx::hle::NotifyGuestWrite(list, L.size);
            ++rewrites;
        } else if (r < 21 && !L.values.empty()) {
            // one value changed (an animated colour, a matrix index)
            uint8_t* v = list + L.values[Rn(static_cast<uint32_t>(L.values.size()))];
            v[3] = static_cast<uint8_t>(v[3] + 1 + Rn(255));
            vgx::hle::NotifyGuestWrite(v, 4);
            ++changes;
        } else if (r < 25) {
            // a write in the same page, not on the list
            uint8_t* v = g_ram + L.addr + L.size + Rn(kListRoom - L.size);
            *v = static_cast<uint8_t>(Rnd());
            vgx::hle::NotifyGuestWrite(v, 1);
            ++neighbours;
        } else if (r < 26) {
            MakeList(l); // (made again: other bytes, maybe another shape)
            ++remade;
        }
        // the vertex format for the call: the list's, now and then another
        int cfg = L.cfg;
        if (Rn(12) == 0) { cfg = static_cast<int>(Rn(4)); if (cfg != L.cfg) ++otherCfg; }
        {
            Bytes f;
            SetCfg(f, cfg);
            Top(f);
        }
        vgx::CallList(list, L.size);
        std::printf("%u %016llx\n", call, static_cast<unsigned long long>(StateHash()));
        if (call % 40 == 0) {
            vgx::g.dispCopySrc = vgx::EfbRect{0, 0, 640, 456};
            vgx::QueueCopyDisp(true);
            vgx::FrontPresent(false, nullptr);
        }
    }
    uint64_t c[5], fp[8];
    vgx::StateListCounters(c);
    vgx::FePlanCounters(fp);
    std::fprintf(stderr, "feplan: %llu replays, %llu plans made, %llu refused (vertex format), %llu vertex size changed; verify: %llu plans checked, %llu mismatching, "
                         "not compared %llu + %llu; state lists: %llu hits, %llu parses, %llu refused; %u rewrites, %u changes, %u writes nearby, %u remade, %u calls "
                         "in another format; %llu draws handed over\n",
                 (unsigned long long)fp[0], (unsigned long long)fp[1], (unsigned long long)fp[2], (unsigned long long)fp[3], (unsigned long long)fp[4],
                 (unsigned long long)fp[5], (unsigned long long)fp[6], (unsigned long long)fp[7], (unsigned long long)c[0], (unsigned long long)c[1],
                 (unsigned long long)c[2], rewrites, changes, neighbours, remade, otherCfg, (unsigned long long)g_draws);
    return 0;
}
