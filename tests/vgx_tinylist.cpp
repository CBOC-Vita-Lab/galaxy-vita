// Tiny state lists through the state-list cache (GXV_TINYLIST, vgx_tinylist.inc, tinylist31) against the plain parser: random tiny display
// lists (at most 64 bytes, CallListBody's tiny branch) in guest memory, at aligned and unaligned addresses, padded and not: J3D's small
// differed lists (a material colour, XF 0x100C, 13 bytes padded to 32; a texture matrix row block and the colour, 62 bytes padded to 64),
// XF-first lists of XF registers, BP writes (behind a mask now and then), CP matrix indices and array bases, indexed matrix loads, NOPs;
// BP-first lists that are not padded material lists; CP-first and indexed-load-first lists; state lists with a draw-sync token; state with a
// draw after it; particle quads (one draw, padding); draws only; a CALL_DL inside; an unknown opcode; a command cut by the end. Called
// through CallList again and again; before a call the lists are written again with the same bytes, one value changed, written next to, or
// made again; the matrix array an indexed load reads is changed now and then. Prints a hash of the whole register state after every call,
// every draw-sync token, and a hash of every command buffer the front end hands over: GXV_TINYLIST=0, 1 and 2 must print the same lines.
// tools/test_vgx_tinylist.sh.
//   vgx_tinylist [calls] [seed]
#include "vgx_hle.h"
#include "vgx_state.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
} // namespace vgx
extern "C" int64_t port_host_time_ns() {
    static int64_t n = 1; // (a counter: runs repeat exactly)
    return n += 1000;
}
extern "C" void port_log_summary(const char* fmt, ...) { // (TINYLIST_LOG=1: the census lines on stderr)
    static const bool on = std::getenv("TINYLIST_LOG") != nullptr;
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
// guest memory: lists from 0x1000 (128 bytes of room each, so that a list's neighbours share its lines and pages), arrays from 0x100000
constexpr uint32_t kListBase = 0x1000, kListRoom = 128, kLists = 96;
constexpr uint32_t kPos = 0x100000, kMtx = 0x160000;
constexpr uint32_t kVerts = 1024;

void PutLE(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
struct Bytes : std::vector<uint8_t> {
    void U8(uint32_t v) { push_back(static_cast<uint8_t>(v)); }
    void U16(uint32_t v) { U8(v >> 8); U8(v); }
    void U32(uint32_t v) { U16(v >> 16); U16(v & 0xFFFF); }
    void BP(uint32_t reg, uint32_t value) { U8(0x61); U32(reg << 24 | (value & 0xFFFFFF)); }
    void CP(uint32_t reg, uint32_t value) { U8(0x08); U8(reg); U32(value); }
    void XF(uint32_t addr, const std::vector<uint32_t>& w) { U8(0x10); U16(static_cast<uint32_t>(w.size() - 1)); U16(addr); for (uint32_t x : w) U32(x); }
};

struct List { uint32_t addr = 0, size = 0; int kind = 0; std::vector<uint32_t> values; };
List g_list[kLists];
uint32_t g_kinds[16] = {};

// one state command (with the value's offset noted for later changes)
void State1(Bytes& b, List& L, uint32_t room) {
    static const uint8_t kBp[] = {0xC0, 0xC1, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0x28, 0x40, 0x41, 0xF3, 0xF6, 0x10, 0x25, 0x30, 0x31};
    auto value = [&] { L.values.push_back(static_cast<uint32_t>(b.size())); };
    const uint32_t r = Rn(12);
    if (r < 4 && room >= 10) {
        if (Rn(8) == 0) b.BP(0xFE, Rnd() & 0xFFFFFF);
        b.U8(0x61); value(); b.U32(static_cast<uint32_t>(kBp[Rn(sizeof kBp)]) << 24 | (Rnd() & 0xFFFFFF));
    } else if (r == 4 && room >= 6) {
        b.U8(0x08); b.U8(Rn(2) ? 0x30 : 0x40); value(); b.U32(Rn(2) ? 0u : Rnd() & 0x3FFFFFFF);
    } else if (r == 5 && room >= 6) {
        b.CP(0xA0, kPos); // (the array base, with the value it holds)
    } else if (r < 9 && room >= 9) {
        const uint32_t addr = 0x1009u + Rn(10);
        b.U8(0x10); b.U16(0); b.U16(addr); value(); b.U32(addr == 0x1009u ? Rn(3) : Rnd());
    } else if (r == 9 && room >= 5) {
        b.U8(0x20); b.U16(Rn(16)); b.U16((11u << 12) | (Rn(10) * 12)); // (a position matrix by its index)
    } else if (r == 10 && room >= 21) {
        b.XF(0x0400 + Rn(8) * 3, {F2u(Rf(-1, 1)), F2u(Rf(-1, 1)), F2u(Rf(-1, 1))}); // (normal matrix words)
    } else {
        b.U8(Rn(2) ? 0x44 : 0x48);
    }
}
void Draw(Bytes& b, uint32_t n) {
    b.U8(Rn(2) ? 0x98 : 0x90); // (format 0: POS, NRM index16, 4 bytes a vertex)
    b.U16(n);
    for (uint32_t i = 0; i < n; ++i) { b.U16(Rn(kVerts)); b.U16(Rn(kVerts)); }
}
void Pad(Bytes& b, uint32_t to) { while (b.size() % to) b.U8(0); }

void MakeList(unsigned l) {
    List& L = g_list[l];
    // (addresses: mostly 32-byte aligned as J3D's lists, some 4- or 8-byte aligned)
    const uint32_t off = Rn(4) == 0 ? (Rn(2) ? 4u : 8u) * (1 + Rn(3)) : 0u;
    L.addr = kListBase + l * kListRoom + off;
    L.values.clear();
    Bytes b;
    const uint32_t k = Rn(16);
    L.kind = static_cast<int>(k);
    ++g_kinds[k];
    auto value = [&] { L.values.push_back(static_cast<uint32_t>(b.size())); };
    switch (k) {
    case 0: case 1: case 2: // J3D: a material colour (13 bytes), padded to 32
        b.U8(0x10); b.U16(0); b.U16(0x100C + Rn(2)); value(); b.U32(Rnd());
        if (k != 2) Pad(b, 32);
        break;
    case 3: case 4: { // J3D: a texture matrix (row block in position matrix memory) and the colour: 62 bytes, padded to 64
        std::vector<uint32_t> w;
        const size_t at = b.size() + 5;
        for (int i = 0; i < 12; ++i) w.push_back(F2u(Rf(-2, 2)));
        b.XF(0x1D * 3 + Rn(3) * 3, w);
        for (int i = 0; i < 12; ++i) L.values.push_back(static_cast<uint32_t>(at + 4 * i));
        b.U8(0x10); b.U16(0); b.U16(0x100C); value(); b.U32(Rnd());
        Pad(b, 64);
        break;
    }
    case 5: case 6: // XF first, then random state commands
        b.U8(0x10); b.U16(0); b.U16(0x1009 + Rn(10)); value(); b.U32(Rn(3));
        for (uint32_t i = 0, n = 1 + Rn(5); i < n && b.size() < 52; ++i) State1(b, L, 64 - static_cast<uint32_t>(b.size()));
        if (Rn(2)) Pad(b, 32);
        break;
    case 7: // BP first, not a padded material list (unpadded, or at an unaligned address)
        b.BP(0xE2, Rnd() & 0x7FFFFF); value();
        for (int i = 0; i < 3; ++i) { b.U8(0x61); value(); b.U32(0xE3u << 24 | (Rnd() & 0x7FFFFF)); }
        if (Rn(2)) Pad(b, 32);
        break;
    case 8: // CP first
        b.U8(0x08); b.U8(0x30); value(); b.U32(Rnd() & 0x3FFFFFFF);
        b.U8(0x10); b.U16(0); b.U16(0x1018); value(); b.U32(Rnd() & 0x3FFFFFFF);
        if (Rn(2)) Pad(b, 32);
        break;
    case 9: // indexed matrix load first
        b.U8(0x20); b.U16(Rn(16)); b.U16((11u << 12) | (Rn(10) * 12));
        State1(b, L, 20);
        Pad(b, 32);
        break;
    case 10: // state, then a draw (refused by the state-list cache: parsed)
        b.U8(0x10); b.U16(0); b.U16(0x100C); value(); b.U32(Rnd());
        Draw(b, 1 + Rn(4));
        Pad(b, 32);
        break;
    case 11: // a particle quad: one draw, padding (jpa_dl)
        Draw(b, 4);
        Pad(b, 32);
        break;
    case 12: // state with a draw-sync token (a trigger: verify must not run it twice)
        b.U8(0x10); b.U16(0); b.U16(0x100C); value(); b.U32(Rnd());
        b.BP(Rn(2) ? 0x47 : 0x48, Rn(0x10000));
        Pad(b, 32);
        break;
    case 13: // XF first, then a CALL_DL inside (ignored by the parse; refused by the cache)
        b.U8(0x10); b.U16(0); b.U16(0x100C); value(); b.U32(Rnd());
        b.U8(0x40); b.U32(kListBase); b.U32(32);
        Pad(b, 32);
        break;
    case 14: // XF first, then an unknown opcode (the parse stops there)
        b.U8(0x10); b.U16(0); b.U16(0x100D); value(); b.U32(Rnd());
        b.U8(0x01 + Rn(3));
        b.U8(0x10); b.U16(0); b.U16(0x100C); b.U32(Rnd());
        break;
    default: // XF first, a command cut by the end (an XF write of 3 words with one there)
        b.U8(0x10); b.U16(0); b.U16(0x100C); value(); b.U32(Rnd());
        b.U8(0x10); b.U16(2); b.U16(0x1009); b.U32(1);
        break;
    }
    L.size = static_cast<uint32_t>(b.size());
    if (L.size > 64 || L.addr + L.size > kListBase + (l + 1) * kListRoom) { std::fprintf(stderr, "list too big (kind %u, %u bytes)\n", k, L.size); std::exit(2); }
    std::memset(g_ram + kListBase + l * kListRoom, 0, kListRoom);
    std::memcpy(g_ram + L.addr, b.data(), L.size);
    vgx::hle::NotifyGuestWrite(g_ram + kListBase + l * kListRoom, kListRoom);
}

uint64_t StateHash() {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&vgx::g);
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < sizeof(vgx::State); ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
uint64_t g_h = 0, g_draws = 0, g_tokens = 0;
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
void Token(uint16_t v, bool irq) {
    ++g_tokens;
    std::printf("token %u%s\n", v, irq ? " irq" : "");
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
    hooks.peToken = Token;
    vgx::SetGuestHooks(hooks);
    vgx::ResetState();
    vgx::SetFrameSink(&Sink);
    vgx::g.efbW = 640;
    vgx::g.efbH = 456;
    vgx::QueueSetTarget();
    for (uint32_t i = 0; i < kVerts * 3; ++i) PutLE(g_ram + kPos + i * 4, F2u(Rf(-1, 1)));
    for (uint32_t i = 0; i < 16 * 12; ++i) PutLE(g_ram + kMtx + i * 4, F2u(Rf(-2, 2)));
    vgx::hle::NotifyGuestWrite(g_ram + kPos, kRam - kPos);
    for (unsigned l = 0; l < kLists; ++l) MakeList(l);
    {
        Bytes f;
        f.CP(0x50, 3u << 9 | 3u << 11); f.CP(0x60, 0); f.CP(0x70, 1u | (4u << 1) | (4u << 10)); f.CP(0x80, 0); f.CP(0x90, 0);
        f.CP(0xA0, kPos); f.CP(0xB0, 12);
        f.CP(0xA1, kPos); f.CP(0xB1, 12);
        f.CP(0xAC, kMtx); f.CP(0xBC, 48);
        f.XF(0x1020, {F2u(1.2f), 0, F2u(1.6f), 0, F2u(-1.0f), F2u(-0.2f), 0});
        f.XF(0x1008, {1u | (1u << 2) | (1u << 4)});
        Top(f);
    }
    unsigned rewrites = 0, changes = 0, neighbours = 0, remade = 0, mtx = 0;
    for (unsigned call = 1; call <= calls; ++call) {
        // (J3D's way: a few lists called in a row, as a picture's packets; a list is mostly called with its last bytes)
        const unsigned l = Rn(kLists);
        List& L = g_list[l];
        uint8_t* const list = g_ram + L.addr;
        const uint32_t r = Rn(100);
        if (r < 15) {
            uint8_t copy[kListRoom];
            std::memcpy(copy, list, L.size);
            std::memcpy(list, copy, L.size);
            vgx::hle::NotifyGuestWrite(list, L.size); // written again with what it holds
            ++rewrites;
        } else if (r < 23 && !L.values.empty()) {
            uint8_t* v = list + L.values[Rn(static_cast<uint32_t>(L.values.size()))];
            v[3] = static_cast<uint8_t>(v[3] + 1 + Rn(255)); // one value changed (an animated colour, a matrix word)
            vgx::hle::NotifyGuestWrite(v, 4);
            ++changes;
        } else if (r < 28) {
            const uint32_t room0 = kListBase + l * kListRoom;
            const uint32_t at = room0 + Rn(kListRoom);
            if (at < L.addr || at >= L.addr + L.size) { // (a write in the same lines, not on the list)
                g_ram[at] = static_cast<uint8_t>(Rnd());
                vgx::hle::NotifyGuestWrite(g_ram + at, 1);
                ++neighbours;
            }
        } else if (r < 30) {
            MakeList(l); // (made again: other bytes, maybe another kind, size or address)
            ++remade;
        } else if (r < 32) {
            const uint32_t at = kMtx + Rn(16 * 12) * 4; // (a matrix an indexed load reads)
            PutLE(g_ram + at, F2u(Rf(-2, 2)));
            vgx::hle::NotifyGuestWrite(g_ram + at, 4);
            ++mtx;
        }
        if (Rn(16) == 0) { // (the top-level stream writes some of the same registers)
            Bytes f;
            f.XF(0x100C, {Rnd()});
            f.BP(0xE2, Rnd() & 0x7FFFFF);
            f.CP(0x30, Rnd() & 0x3FFFFFFF);
            Top(f);
        }
        vgx::CallList(g_ram + g_list[l].addr, g_list[l].size);
        std::printf("%u %016llx\n", call, static_cast<unsigned long long>(StateHash()));
        if (call % 10 == 0) { // (a picture every 10 calls: the back-offs (300, 600 pictures) run out within a run)
            vgx::g.dispCopySrc = vgx::EfbRect{0, 0, 640, 456};
            vgx::QueueCopyDisp(true);
            vgx::FrontPresent(false, nullptr);
        }
    }
    uint64_t c[5], tl[8];
    vgx::StateListCounters(c);
    vgx::TinyListCounters(tl);
    std::fprintf(stderr, "tinylist: %llu fast replays, %llu StateListCall replays, %llu refused, %llu backed off; verify: %llu checked, %llu mismatching, "
                         "not compared %llu trigger + %llu not replayed; state lists: %llu hits, %llu parses, %llu refused; %u rewrites, %u changes, %u writes nearby, "
                         "%u remade, %u matrix writes; %llu tokens, %llu draws handed over; kinds",
                 (unsigned long long)tl[0], (unsigned long long)tl[1], (unsigned long long)tl[2], (unsigned long long)tl[3], (unsigned long long)tl[4],
                 (unsigned long long)tl[5], (unsigned long long)tl[6], (unsigned long long)tl[7], (unsigned long long)c[0], (unsigned long long)c[1],
                 (unsigned long long)c[2], rewrites, changes, neighbours, remade, mtx, (unsigned long long)g_tokens, (unsigned long long)g_draws);
    for (uint32_t k : g_kinds) std::fprintf(stderr, " %u", k);
    std::fprintf(stderr, "\n");
    return 0;
}
