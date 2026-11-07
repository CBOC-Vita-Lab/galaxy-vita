// Host implementations of the kernel calls in tests/par_stubs/psp2 (std::thread and counting semaphores), so that
// tests/par_exec_view.cpp can run the console branch of platform/include/port/par_exec.h (tools/test_par_exec_view.sh).
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
using SceUID = std::int32_t;
using SceSize = std::uint32_t;
using SceUInt = std::uint32_t;
using SceUInt64 = std::uint64_t;
typedef int (*SceKernelThreadEntry)(SceSize args, void* argp);
namespace {
struct Sem {
    std::mutex m;
    std::condition_variable cv;
    int count = 0;
};
// (never destroyed: the worker thread waits on these until the process ends)
std::mutex& gLock = *new std::mutex;
std::vector< Sem* >& gSems = *new std::vector< Sem* >;
struct Th {
    SceKernelThreadEntry entry;
};
std::vector< Th >& gThreads = *new std::vector< Th >;
}  // namespace
extern "C" {
SceUID sceKernelCreateSema(const char*, SceUInt, int initVal, int, const void*) {
    std::lock_guard< std::mutex > l(gLock);
    Sem* s = new Sem;
    s->count = initVal;
    gSems.push_back(s);
    return static_cast< SceUID >(gSems.size() - 1);
}
int sceKernelWaitSema(SceUID id, int signal, SceUInt*) {
    Sem* s;
    {
        std::lock_guard< std::mutex > l(gLock);
        s = gSems[id];
    }
    std::unique_lock< std::mutex > l(s->m);
    s->cv.wait(l, [&] { return s->count >= signal; });
    s->count -= signal;
    return 0;
}
int sceKernelSignalSema(SceUID id, int signal) {
    Sem* s;
    {
        std::lock_guard< std::mutex > l(gLock);
        s = gSems[id];
    }
    std::lock_guard< std::mutex > l(s->m);
    s->count += signal;
    s->cv.notify_all();
    return 0;
}
SceUID sceKernelCreateThread(const char*, SceKernelThreadEntry entry, int, SceSize, SceUInt, int, const void*) {
    std::lock_guard< std::mutex > l(gLock);
    gThreads.push_back(Th{entry});
    return static_cast< SceUID >(0x40010000 + gThreads.size() - 1);
}
int sceKernelStartThread(SceUID uid, SceSize arglen, void* argp) {
    SceKernelThreadEntry entry;
    {
        std::lock_guard< std::mutex > l(gLock);
        entry = gThreads[uid - 0x40010000].entry;
    }
    std::vector< unsigned char > copy(static_cast< const unsigned char* >(argp), static_cast< const unsigned char* >(argp) + arglen);
    std::thread([entry, copy]() mutable { entry(static_cast< SceSize >(copy.size()), copy.data()); }).detach();
    return 0;
}
int sceKernelGetThreadCurrentPriority(void) { return 0x9F; }
SceUInt64 sceKernelGetProcessTimeWide(void) {
    return static_cast< SceUInt64 >(
        std::chrono::duration_cast< std::chrono::microseconds >(std::chrono::steady_clock::now().time_since_epoch()).count());
}
}
