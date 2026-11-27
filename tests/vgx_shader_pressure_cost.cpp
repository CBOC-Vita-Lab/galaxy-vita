#define main quiet_fixture_main
#define VGX_TEST_NATIVE_CAPACITY
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "vgx_backend_quiet.cpp"
#pragma clang diagnostic pop
#undef main
static unsigned queries = 0, deletionCalls = 0;
extern "C" unsigned vglGetFreeProgramSlots() { ++queries; return 124; }
extern "C" void __wrap_glDeleteProgram(GLuint) { ++deletionCalls; }
int main() {
    using namespace vgx::backend;
    setenv("GXV_MAXPROGRAMS", "900", 1);
    for (unsigned i = 0; i < 900; ++i) {
        vgx::backend::Program p; p.id = i + 1; p.ok = true; p.usedSerial = p.linkSerial = 1000;
        g_programs.emplace(i, p);
    }
#ifndef BASELINE_COST
    g_nativeCapacityPressure = true;
#endif
    for (unsigned sample = 0; sample < 5; ++sample) {
        queries = 0;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned repeat = 0; repeat < 200; ++repeat) {
#ifdef PRESSURE_SCAN_CACHE
            g_capacityNextScanSerial = 0;
#endif
            for (g_serial = 1000; g_serial <= 1600; ++g_serial) EvictPrograms();
        }
        const double ns = std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count();
        char line[256]; const int n = std::snprintf(line,sizeof line,
            "sample=%u frames=120200 resident=900 zero_eligible=1 ns_per_boundary=%.3f queries=%u deletes=%u physical_vita=false\n",
            sample, ns / 120200.0, queries, deletionCalls);
        ::write(STDOUT_FILENO,line,n);
        Require(deletionCalls == 0 && g_programs.size() == 900, "protected benchmark never retires names");
    }
}
