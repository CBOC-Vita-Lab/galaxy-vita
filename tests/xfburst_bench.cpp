// Bounded front-end microcheck. Actual C GX entrypoint and actual FIFO writer;
// only external renderer publication is a no-op consumer. No game timing claim.
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "../platform/src/gx/gx_fifo.cpp"
#include "revolution/gx/GXRegs.h"

static GXData gxState;
extern "C" GXData* const __GXData = &gxState;
static unsigned publications;
extern "C" int port_vgx_player() { return 1; }
extern "C" int port_vgx_active() { return 1; }
extern "C" uint8_t* port_vgx_publish(uint8_t* p, int flags) {
    ++publications;
    return flags & 1 ? sStagingOther : p;
}
extern "C" size_t port_vgx_consume(const uint8_t*, size_t n) { return n; }
extern "C" void port_vgx_stream_setup(uint8_t*, uint8_t*, size_t) {}
extern "C" void port_perf_record(int64_t) {}
extern "C" int64_t port_host_time_ns() { return 1; }
extern "C" void* port_host_alloc(size_t n, int alignment) {
    void* p = nullptr; if (posix_memalign(&p, alignment, n)) std::abort(); return p;
}
extern "C" void port_fatal(const char* message, ...) { fprintf(stderr, "fatal: %s\n", message); std::abort(); }
extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char*, ...) {}
extern "C" void port_host_alloc_scope_enter() {}
extern "C" void port_host_alloc_scope_leave() {}
namespace gpu { size_t process(const uint8_t*, size_t n, bool) { return n; } }

static uint64_t ns(clockid_t clock) {
    timespec t{}; if (clock_gettime(clock, &t)) { perror("clock_gettime"); std::abort(); }
    return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    setenv("GXV_XFBURST", argv[1], 1);
    const unsigned count = strtoul(argv[2], nullptr, 10);
    if (!count || count > 5000000) return 2;
    sStaging = static_cast<uint8_t*>(port_host_alloc(kStagingSize + 4096, 64));
    sStagingOther = static_cast<uint8_t*>(port_host_alloc(kStagingSize + 4096, 64));
    memset(sStaging, 0, kStagingSize); memset(sStagingOther, 0, kStagingSize);
    sLinked = true; sCPUReady = true; sGPReady = true;
    sLinkedPtr = __PortGXFifoPtr = sStaging;
    setLinkedLimit();
    alignas(16) float source[3][4] = {{1,2,3,4},{5,6,7,8},{9,10,11,12}};
    for (unsigned i = 0; i < 10000; ++i) GXLoadPosMtxImm(source, 7);
    publications = 0;
    const uint64_t wall = ns(CLOCK_MONOTONIC), cpu = ns(CLOCK_THREAD_CPUTIME_ID);
    for (unsigned i = 0; i < count; ++i) GXLoadPosMtxImm(source, 7);
    const uint64_t elapsedCpu = ns(CLOCK_THREAD_CPUTIME_ID) - cpu;
    const uint64_t elapsedWall = ns(CLOCK_MONOTONIC) - wall;
    uint64_t checksum = 0;
    for (unsigned i = 0; i < 48; ++i) checksum = checksum * 31 + __PortGXFifoPtr[int(i) - 48];
    printf("mode=%s calls=%u cpu_ns=%llu wall_ns=%llu publications=%u checksum=%llu\n",
        argv[1], count, (unsigned long long)elapsedCpu, (unsigned long long)elapsedWall,
        publications, (unsigned long long)checksum);
}
