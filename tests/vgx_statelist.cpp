// (From Producdevity's fork, b7f30ea; public domain. Passes against our state-list cache in GXV_STATELIST=0/1/2, also with GXV_TEVIN_CANON=1/2,
// GXV_LIGHT_DEDUP=2 and the FE switches; forkcheck23, 8 Nov.)
// The state-list cache (vgx_state.cpp: StateListFastHit, StateListCall) against the plain parser: material-like display lists in
// guest memory, called again and again through CallList, now and then written again with the same bytes, with a register value
// changed, or written next to. Prints a hash of the whole register state after every call: GXV_STATELIST=0 (every call parsed),
// 1 and 2 must print the same lines. tools/test_vgx_statelist.sh.
#include "vgx_hle.h"
#include "vgx_state.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
} // namespace vgx

namespace {
uint64_t g_rng = 0x9E3779B97F4A7C15ull;
uint32_t Rnd() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return static_cast<uint32_t>(g_rng >> 11);
}
uint32_t Rn(uint32_t n) { return Rnd() % n; }

constexpr unsigned kLists = 8, kSlot = 512;
alignas(4096) uint8_t g_ram[3 * 4096];
uint32_t g_size[kLists];
uint32_t g_value[kLists][64]; // byte offset of each value word that may change
unsigned g_values[kLists];

void Put32(uint8_t* p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}
// A list of the commands J3D's material lists hold: BP writes (TEV stages and colours, blend, z, fog and indirect registers, a
// mask write now and then), CP matrix indices, XF colour / texgen registers and matrix rows; zero padding to 32 bytes.
void MakeList(unsigned l) {
    static const uint8_t kBp[] = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7,
                                  0x28, 0x29, 0x40, 0x41, 0x42, 0xF3, 0xF6, 0xF7, 0x10, 0x11, 0x25, 0x26};
    uint8_t* const base = g_ram + l * kSlot;
    uint8_t* p = base;
    g_values[l] = 0;
    const unsigned cmds = 12 + Rn(20);
    for (unsigned c = 0; c < cmds; ++c) {
        const uint32_t r = Rn(10);
        if (r < 6) {
            if (Rn(12) == 0) {
                *p++ = 0x61;
                Put32(p, 0xFEu << 24 | (Rnd() & 0xFFFFFF));
                p += 4;
            }
            *p++ = 0x61;
            g_value[l][g_values[l]++] = static_cast<uint32_t>(p - base);
            Put32(p, static_cast<uint32_t>(kBp[Rn(sizeof kBp)]) << 24 | (Rnd() & 0xFFFFFF));
            p += 4;
        } else if (r < 7) {
            *p++ = 0x08;
            *p++ = Rn(2) ? 0x30 : 0x40;
            g_value[l][g_values[l]++] = static_cast<uint32_t>(p - base);
            Put32(p, Rnd());
            p += 4;
        } else if (r < 9) {
            const uint32_t addr = 0x1009u + Rn(10);
            *p++ = 0x10;
            *p++ = 0;
            *p++ = 0;
            *p++ = static_cast<uint8_t>(addr >> 8);
            *p++ = static_cast<uint8_t>(addr);
            g_value[l][g_values[l]++] = static_cast<uint32_t>(p - base);
            Put32(p, addr == 0x1009u ? Rn(3) : Rnd());
            p += 4;
        } else {
            const uint32_t row = Rn(20) * 3;
            *p++ = 0x10;
            *p++ = 0;
            *p++ = 11;
            *p++ = static_cast<uint8_t>((row * 4) >> 8);
            *p++ = static_cast<uint8_t>(row * 4);
            for (unsigned k = 0; k < 12; ++k) {
                float f = static_cast<float>(static_cast<int>(Rn(2001)) - 1000) / 250.0f;
                uint32_t u;
                std::memcpy(&u, &f, 4);
                Put32(p, u);
                p += 4;
            }
        }
    }
    while ((p - base) % 32) *p++ = 0;
    g_size[l] = static_cast<uint32_t>(p - base);
}

uint64_t StateHash() {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&vgx::g);
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < sizeof(vgx::State); ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
void Sink(vgx::CmdBuf& b) { vgx::backend::Execute(b); }
} // namespace

int main() {
    vgx::ResetState();
    vgx::SetFrameSink(&Sink);
    vgx::g.efbW = 640;
    vgx::g.efbH = 456;
    vgx::QueueSetTarget();
    for (unsigned l = 0; l < kLists; ++l) MakeList(l);
    vgx::hle::RegisterGuestRam(g_ram, sizeof g_ram);
    unsigned rewrites = 0, changes = 0, neighbours = 0;
    for (unsigned call = 1; call <= 6000; ++call) {
        const unsigned l = Rn(kLists);
        uint8_t* const list = g_ram + l * kSlot;
        const uint32_t r = Rn(100);
        if (r < 20) {
            // written again with what it holds (J3D's update of a material list)
            uint8_t copy[kSlot];
            std::memcpy(copy, list, g_size[l]);
            std::memcpy(list, copy, g_size[l]);
            vgx::hle::NotifyGuestWrite(list, g_size[l]);
            ++rewrites;
        } else if (r < 26 && g_values[l]) {
            // one register value changed (an animated colour)
            uint8_t* v = list + g_value[l][Rn(g_values[l])];
            v[3] = static_cast<uint8_t>(v[3] + 1 + Rn(255));
            vgx::hle::NotifyGuestWrite(v, 4);
            ++changes;
        } else if (r < 30) {
            // a write in the same page, not on the list
            uint8_t* v = g_ram + 2 * 4096 + Rn(4096);
            *v = static_cast<uint8_t>(Rnd());
            vgx::hle::NotifyGuestWrite(v, 1);
            ++neighbours;
        }
        vgx::CallList(list, g_size[l]);
        std::printf("%u %016llx\n", call, static_cast<unsigned long long>(StateHash()));
        if (call % 40 == 0) {
            vgx::g.dispCopySrc = vgx::EfbRect{0, 0, 640, 456};
            vgx::QueueCopyDisp(true);
            vgx::FrontPresent(false, nullptr);
        }
    }
    uint64_t c[5];
    vgx::StateListCounters(c);
    std::fprintf(stderr, "statelist: %llu hits, %llu parses, %llu refused, %llu verified, %llu mismatching; %u rewrites, %u changes, %u writes nearby\n",
                 static_cast<unsigned long long>(c[0]), static_cast<unsigned long long>(c[1]), static_cast<unsigned long long>(c[2]),
                 static_cast<unsigned long long>(c[3]), static_cast<unsigned long long>(c[4]), rewrites, changes, neighbours);
    return 0;
}
