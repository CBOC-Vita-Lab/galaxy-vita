// psp2 kernel shim for the armhf PGO harness (see vita_shim.h): the host's clock and sleep, what the code took before the shim.
#include <chrono>
#include <thread>

extern "C" {
unsigned int g_vgx_arena_min = 0;
unsigned long long sceKernelGetProcessTimeWide(void) {
    return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
int sceKernelDelayThread(unsigned int usec) {
    std::this_thread::sleep_for(std::chrono::microseconds(usec));
    return 0;
}
}
