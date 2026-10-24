// Count allocations in the real variant bookkeeping, not a GPU benchmark.
extern "C" { unsigned char vgxvita_maskGuardDue = 0; }
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#include <cstdio>
#include <cstdlib>

namespace vgx { bool LogEnabled() { return false; } }
namespace {
bool measuring = false;
struct Allocations { size_t calls = 0, bytes = 0; } allocations;
uint64_t serialBase = 100;
void Require(bool good, const char* why) {
    if (!good) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
Allocations Sweep(vgx::backend::Program& p, bool changing) {
    allocations = {}; measuring = true;
    for (unsigned i = 0; i < 8192; ++i) {
        const uint32_t layout = changing ? i & 4095u : 4095u;
        const uint32_t blend = changing && !(i & 1u) ? 0xc00u : 0xcc9u;
        vgx::backend::g_serial = serialBase + i / 8;
        vgx::backend::NoteVariant(p, 1, 2, layout, blend, true);
    }
    measuring = false; serialBase += 1024;
    return allocations;
}
}
extern "C" void* __real__Znwm(size_t);
extern "C" void* __wrap__Znwm(size_t size) {
    if (measuring) { ++allocations.calls; allocations.bytes += size; }
    return __real__Znwm(size);
}
int main() {
    using namespace vgx::backend;
    Program p;
    const auto cold = Sweep(p, true), repeat = Sweep(p, false), alternating = Sweep(p, true);
    Require(p.layout == 4095 && p.blendKey == 0xcc9 && p.warmCompatible && p.usedSerial == 3171,
            "quiet bookkeeping preserves layout, blend, warm compatibility and eviction age");
    Require(g_progsThisFrame == 3072, "program-use frame accounting is preserved");
    Require(C.fragSwitches == 16383, "blend-switch accounting survives all three sweeps");
    Require(!repeat.calls && !alternating.calls, "already-seen variants do not allocate");
    Require(C.glCalls == 0, "variant bookkeeping issues no GL calls");
#if defined(PORT_RUNTIME_DIAGNOSTICS) && !PORT_RUNTIME_DIAGNOSTICS
    constexpr bool quiet = true;
    Require(cold.calls == 0 && C.fragVariants == 0 && C.vtxVariants == 0,
            "diagnostics0 omits variant-census allocations and counts");
#else
    constexpr bool quiet = false;
    Require(cold.calls >= 4098 && C.fragVariants == 2 && C.vtxVariants == 4096,
            "undefined/diagnostics1 retains the legacy census");
#endif
    p.layoutNeed=vgx::VL_NRM;
    for (unsigned i=0;i<4;++i) NoteVariant(p,1,2,0,0xcc9,true);
    Require(C.constAttrDraws==4, "missing-attribute accounting survives quiet census removal");
    std::printf("PASS: variant census quiet=%u cold_new=%zu requested_bytes=%zu repeat_new=%zu alternating_new=%zu (not Vita FPS)\n",
                quiet, cold.calls, cold.bytes, repeat.calls, alternating.calls);
}
