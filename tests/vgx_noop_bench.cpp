#include <cstdarg>
// What a register write costs the front end's top-level parse, by class and whether it changes anything (fedl25, GXV_NOOP_FILTER, GXV_STREAM_FILTER).
// Streams of N commands of one class go through the real vgx::ProcessStream (top level, no lists, no draws); the time is the best of many runs
// divided by N (host ns per command; the Vita is several times slower, the ratios carry over), and under valgrind --tool=callgrind the instruction
// count of one run (FEWB_REPS=1, FEWB_N=100000) gives the same number load-independent.
//   vgx_noop_bench [N=4096] [reps=200]
//   FEWB_ONLY=<name substring>, FEWB_NOHDR=1 (no header line)
//   FEWB_VERIFY=1: after every case the register file is compared with the one the same stream made with GXV_NOOP_FILTER=0 (only meaningful
//                  when run twice and the "state hash" lines compared: tools/test_vgx_noopfilter.sh)
#include "vgx_state.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"
#include "vgx_cmd.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}  // namespace vgx
namespace {
using Bytes = std::vector<uint8_t>;
uint8_t g_ram[1 << 16];
const uint8_t* Resolve(uint32_t, uint32_t* avail) { *avail = 0; return nullptr; }
void Be32(Bytes& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s)); }
void Bp(Bytes& b, uint32_t reg, uint32_t val) { b.push_back(0x61); Be32(b, (reg << 24) | (val & 0xFFFFFF)); }
void Cp(Bytes& b, uint32_t reg, uint32_t val) { b.push_back(0x08); b.push_back(static_cast<uint8_t>(reg)); Be32(b, val); }
void Xf(Bytes& b, uint32_t addr, const std::vector<uint32_t>& w) {
    b.push_back(0x10);
    b.push_back(0); b.push_back(static_cast<uint8_t>((w.size() - 1) & 0xF));
    b.push_back(static_cast<uint8_t>(addr >> 8)); b.push_back(static_cast<uint8_t>(addr));
    for (uint32_t v : w) Be32(b, v);
}
uint32_t g_rng = 12345;
uint32_t Rn() { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

struct Case { const char* name; Bytes prime, body; uint32_t cmds; };

// Registers of the J3D material / shape brackets: plain BP registers (0x28.. TEV, 0x40.. etc. are all "plain" apart from the table's few), the CP array
// bases and strides, CP 0x30 / 0x40, XF position matrix memory, XF registers 0x1040.. (not the matrix indices 0x1018 / 0x1019).
std::vector<Case> Cases(uint32_t n) {
    std::vector<Case> c;
    auto mk = [&](const char* name, auto prime, auto body) {
        Case k; k.name = name; prime(k.prime); body(k.body, k.cmds); c.push_back(std::move(k));
    };
    // BP
    mk("bp-noop-burst", [&](Bytes& b) { for (uint32_t r = 0; r < 8; ++r) Bp(b, 0x28 + r, 0x1230 + r); },
       [&](Bytes& b, uint32_t& cm) { for (uint32_t i = 0; i < n; ++i) Bp(b, 0x28 + (i & 7), 0x1230 + (i & 7)); cm = n; });
    mk("bp-noop-alternating-cp", [&](Bytes& b) { for (uint32_t r = 0; r < 8; ++r) Bp(b, 0x28 + r, 0x1230 + r); Cp(b, 0xA0, 0x8000); },
       [&](Bytes& b, uint32_t& cm) { for (uint32_t i = 0; i < n / 2; ++i) { Bp(b, 0x28 + (i & 7), 0x1230 + (i & 7)); Cp(b, 0xA0, 0x8000); } cm = n / 2 * 2; });
    mk("bp-change", [&](Bytes&) {},
       [&](Bytes& b, uint32_t& cm) { for (uint32_t i = 0; i < n; ++i) Bp(b, 0x28 + (i & 7), (Rn() & 0xFFFF) | 0x10000); cm = n; });
    // CP
    mk("cp-arraybase-noop", [&](Bytes& b) { for (uint32_t r = 0; r < 4; ++r) Cp(b, 0xA0 + r, 0x8000 + r * 64); },
       [&](Bytes& b, uint32_t& cm) { for (uint32_t i = 0; i < n; ++i) Cp(b, 0xA0 + (i & 3), 0x8000 + (i & 3) * 64); cm = n; });
    mk("cp-arraybase-change", [&](Bytes&) {},
       [&](Bytes& b, uint32_t& cm) { for (uint32_t i = 0; i < n; ++i) Cp(b, 0xA0 + (i & 3), 0x8000 + (Rn() & 0xFFF) * 32); cm = n; });
    mk("cp-matidx-noop", [&](Bytes& b) { Cp(b, 0x30, 0x3C3C3C3C); },
       [&](Bytes& b, uint32_t& cm) { for (uint32_t i = 0; i < n; ++i) Cp(b, 0x30, 0x3C3C3C3C); cm = n; });
    mk("cp-vcd-noop", [&](Bytes& b) { Cp(b, 0x50, 0x00000C00); Cp(b, 0x60, 0x0003FFFF); },
       [&](Bytes& b, uint32_t& cm) { for (uint32_t i = 0; i < n; ++i) Cp(b, 0x50 + 0x10 * (i & 1), (i & 1) ? 0x0003FFFFu : 0x00000C00u); cm = n; });
    // XF
    {
        std::vector<uint32_t> m(12);
        for (uint32_t i = 0; i < 12; ++i) m[i] = 0x3F800000u + i * 0x1000u;
        mk("xf-posmtx12-noop", [&](Bytes& b) { Xf(b, 0, m); },
           [&](Bytes& b, uint32_t& cm) { for (uint32_t i = 0; i < n; ++i) Xf(b, 0, m); cm = n; });
        mk("xf-posmtx12-change", [&](Bytes&) {},
           [&](Bytes& b, uint32_t& cm) { for (uint32_t i = 0; i < n; ++i) { m[0] = 0x3F800000u + (Rn() & 0xFFFF); Xf(b, 12 * (i & 7), m); } cm = n; });
        std::vector<uint32_t> r4(4, 0x40000000u);
        mk("xf-reg4-noop", [&](Bytes& b) { Xf(b, 0x1040, r4); },
           [&](Bytes& b, uint32_t& cm) { for (uint32_t i = 0; i < n; ++i) Xf(b, 0x1040, r4); cm = n; });
        std::vector<uint32_t> r1(1, 0x41000000u);
        mk("xf-reg1-noop", [&](Bytes& b) { Xf(b, 0x1043, r1); },
           [&](Bytes& b, uint32_t& cm) { for (uint32_t i = 0; i < n; ++i) Xf(b, 0x1043, r1); cm = n; });
    }
    // the shape of a J3D shape bracket: 6 array writes, matidx, one matrix load; the value pattern of consecutive shapes of one material (most of it the same)
    {
        std::vector<uint32_t> m(12, 0x3F000000u);
        mk("bracket-same", [&](Bytes& b) {
               for (uint32_t r = 0; r < 4; ++r) { Cp(b, 0xA0 + r, 0x8000 + r * 64); Cp(b, 0xB0 + r, 12); }
               Cp(b, 0x30, 0x3C3C3C3C); Cp(b, 0x40, 0x3C3C3C3C); Xf(b, 0, m); },
           [&](Bytes& b, uint32_t& cm) {
               uint32_t k = 0;
               for (uint32_t i = 0; i < n / 14; ++i) {
                   for (uint32_t r = 0; r < 4; ++r) { Cp(b, 0xA0 + r, 0x8000 + r * 64); Cp(b, 0xB0 + r, 12); }
                   Cp(b, 0x30, 0x3C3C3C3C); Cp(b, 0x40, 0x3C3C3C3C); Xf(b, 0, m); k += 11;
               }
               cm = k; });
    }
    // a mix of every kind with values from a small set, so that about half of the writes repeat what the register holds (the filter's test: the
    // register file after the stream must be the same with the filter on and off)
    mk("mixed-random", [&](Bytes&) {},
       [&](Bytes& b, uint32_t& cm) {
           for (uint32_t i = 0; i < n; ++i) {
               const uint32_t k = Rn() % 14, v = Rn() % 3;
               switch (k) {
               case 0: case 1: Bp(b, 0x28 + Rn() % 8, 0x1230 + v); break;
               case 2: Bp(b, 0xC0 + Rn() % 8, 0x2000 + v); break;
               case 3: Bp(b, 0xE0 + Rn() % 8, 0x300 + v); break;
               case 4: Cp(b, 0xA0 + Rn() % 4, 0x8000 + v * 64); break;
               case 5: Cp(b, 0xB0 + Rn() % 4, 12 + v); break;
               case 6: Cp(b, Rn() & 1 ? 0x30 : 0x40, 0x3C3C3C00u + v); break;
               case 7: { std::vector<uint32_t> m(12, 0x3F000000u + v); Xf(b, 12 * (Rn() % 4), m); break; }
               case 8: { std::vector<uint32_t> m(4, 0x3F000000u + v); Xf(b, 0x1040 + (Rn() & 3), m); break; }
               case 9: { std::vector<uint32_t> m(1, 0x3C3C3C00u + v); Xf(b, 0x1018 + (Rn() & 1), m); break; }
               case 10: { std::vector<uint32_t> m(4, 0x3F800000u + v); Xf(b, 0x600 + 16 * (Rn() & 3), m); break; }
               case 11: { std::vector<uint32_t> m(9, 0x3F000000u + v); Xf(b, 0x400 + 9 * (Rn() & 1), m); break; }
               case 12: { std::vector<uint32_t> m(12, 0x40000000u + v); Xf(b, 0x500 + 12 * (Rn() & 3), m); break; }
               default: { std::vector<uint32_t> m(Rn() % 16 + 1, 0x3F000000u + v); Xf(b, 0x1000 + Rn() % 0x40, m); break; }
               }
           }
           cm = n; });
    return c;
}
uint64_t StateHash() {
    // the parts of the register file the cases write
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (int i = 0; i < 256; ++i) mix(vgx::g.bp[i]);
    for (int i = 0; i < 16; ++i) { mix(vgx::g.cpBase[i]); mix(vgx::g.cpStride[i]); }
    mix(vgx::g.matIdxA); mix(vgx::g.matIdxB);
    for (int r = 0; r < 64; ++r) for (int k = 0; k < 4; ++k) { uint32_t w; std::memcpy(&w, &vgx::g.xfPos[r][k], 4); mix(w); }
    for (int i = 0; i < 0x58; ++i) mix(vgx::g.xfReg[i]);
    mix(vgx::g.xfPosRowsDirty); mix(vgx::g.xfMtxDirty); mix(vgx::g.bpDirty); mix(vgx::g.cpArrayStale);
    return h;
}
}  // namespace

extern "C" void port_vgx_verify_summary(void);
extern "C" void port_log_summary(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
}
int main(int argc, char** argv) {
    const uint32_t n = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 4096;
    const int reps = argc > 2 ? std::atoi(argv[2]) : 200;
    const char* only = std::getenv("FEWB_ONLY");
    const bool verify = std::getenv("FEWB_VERIFY") != nullptr;
    vgx::hle::RegisterGuestRam(g_ram, sizeof g_ram);
    vgx::GuestHooks hooks{};
    hooks.resolve = Resolve;
    vgx::SetGuestHooks(hooks);
    if (!std::getenv("FEWB_NOHDR")) std::printf("%-26s %8s %10s\n", "case", "cmds", "ns/cmd");
    for (Case& k : Cases(n)) {
        if (only && !std::strstr(k.name, only)) continue;
        vgx::ResetState();
        vgx::g.efbW = 640; vgx::g.efbH = 456;
        if (!k.prime.empty()) { uint32_t r = 0; while (r < k.prime.size()) r += vgx::ProcessStream(k.prime.data() + r, static_cast<uint32_t>(k.prime.size() - r), false); }
        double best = 1e30;
        for (int rep = 0; rep < reps; ++rep) {
            const auto t0 = std::chrono::steady_clock::now();
            uint32_t r = 0;
            while (r < k.body.size()) {
                const uint32_t u = vgx::ProcessStream(k.body.data() + r, static_cast<uint32_t>(k.body.size() - r), false);
                if (!u) { std::fprintf(stderr, "stuck in %s\n", k.name); return 2; }
                r += u;
            }
            const auto t1 = std::chrono::steady_clock::now();
            best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / k.cmds);
        }
        std::printf("%-26s %8u %10.2f", k.name, k.cmds, best);
        if (verify) std::printf("   state hash %016llx", static_cast<unsigned long long>(StateHash()));
        std::printf("\n");
    }
    {
        uint64_t nf[2];
        vgx::NoopFilterCounters(nf);
        if (nf[0] || verify) std::printf("noopfilter: %llu checked, %llu mismatching\n", static_cast<unsigned long long>(nf[0]), static_cast<unsigned long long>(nf[1]));
        port_vgx_verify_summary();  // (verifyfix31: the game-log line: "verify: noopfilter: ... N checked, M mismatching")
    }
    return 0;
}
