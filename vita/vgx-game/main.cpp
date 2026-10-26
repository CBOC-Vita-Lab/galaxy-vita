// Entry of the vgx build (title SMGV00001): the sceGxm worker of vita/game is gone, vitaGL is
// started by the renderer's own thread (platform/vgx/vita/vgx_host_galaxy_vita.cpp).
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

// The C heap (newlib): host-only allocations (renderer, std containers, the platform's own
// bookkeeping; the game's own `new` goes to its JKR heaps). The default is 32 MiB. Memory plan:
// notes/vita-bringup.md.
//
// 56 MiB was not enough on the console: the opening demo's scene change took the heap from 11 to
// 40 MiB in use (53.8 MiB of arena) within seconds, and the next allocation failed (the game's
// `new` used to return null there, now it reports and aborts). The console reports 205 MiB of
// free main memory before vitaGL starts and 84 MiB at the end of start-up, so 96 MiB still
// leaves 44 MiB. -DVITA_CHEAP_MIB=<n> sets it for a build; the heap's growth is logged
// ("host heap: arena ...") to tune it.
#ifndef VITA_CHEAP_MIB
#define VITA_CHEAP_MIB 96
#endif
extern "C" unsigned int _newlib_heap_size_user = static_cast<unsigned int>(VITA_CHEAP_MIB) * 1024u * 1024u;

// VitaSDK's pthread library waits for a semaphore "cancellably" by POLLING: it wakes every
// 500 microseconds to look at the thread's cancel flag (pte_osSemaphoreCancellablePend), and
// every pthread_cond_wait / std::condition_variable wait goes through it. The game has a
// dozen threads that sit in such a wait almost all the time (only one of them owns the
// "CPU" at any moment), all on the game's core: about 25,000 wake-ups a second for nothing.
// Nothing here cancels threads, so the wait is a plain blocking one (the linker's
// --wrap=pte_osSemaphoreCancellablePend sends the library's calls here).
// Results are pte_osResult values: 0 OK, 2 failure, 3 timeout. The timeout is in milliseconds.
extern "C" int __wrap_pte_osSemaphoreCancellablePend(SceUID semaphore, unsigned int* timeoutMs) {
    int result;
    if (timeoutMs) {
        SceUInt us = *timeoutMs > 4000000u ? 4000000000u : *timeoutMs * 1000u;
        result = sceKernelWaitSema(semaphore, 1, &us);
    } else {
        result = sceKernelWaitSema(semaphore, 1, nullptr);
    }
    if (result >= 0) return 0;
    return result == static_cast<int>(0x80028005) ? 3 : 2; // SCE_KERNEL_ERROR_WAIT_TIMEOUT
}

extern "C" int port_vita_game_entry(int argc, char** argv);
extern "C" void port_log_flush(void);
int main(int argc, char** argv) {
    const int result = port_vita_game_entry(argc, argv);
    port_log_flush();  // (the log is written in batches: what is still waiting goes to the card before the process ends)
    sceKernelExitProcess(result);
    return result;
}
