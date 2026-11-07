// Link-time stand-ins for what platform/vgx/src/vgx_autoq.cpp calls when a test builds the back end with __VGX_VITA__ and links
// autoq in (the back end reaches its Log/Target30/clock functions). Separate translation unit on purpose: it sees no SDK header,
// so it cannot disagree with whichever header set the test itself uses. The clock watcher thread is never started by these tests.
#include <chrono>
#include <cstdlib>
extern "C" {
void port_log(const char*, ...) {}
void port_log_summary(const char*, ...) {}
// Weak: a test that controls the process clock defines its own.
__attribute__((weak)) unsigned long long sceKernelGetProcessTimeWide(void) {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int sceKernelCreateSema(const char*, unsigned, int, int, const void*) { return -1; }
int sceKernelWaitSema(int, int, unsigned*) { std::abort(); }
int sceKernelSignalSema(int, int) { std::abort(); }
int sceKernelCreateThread(const char*, void*, int, unsigned, unsigned, int, const void*) { std::abort(); }
int sceKernelStartThread(int, unsigned, void*) { std::abort(); }
int sceKernelDelayThread(unsigned) { std::abort(); }
}
