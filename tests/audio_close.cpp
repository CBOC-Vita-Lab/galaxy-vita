// vita/runtime/audio.cpp: closing the output while its thread is stuck in the driver (a port the system no longer serves after the
// console woke up) must not wait for ever, and a resume report is counted.  Fake system calls; no thread is really started.
#include <atomic>
#include <cassert>
#include <cstdio>
#include "../vita/runtime/audio.cpp"

static int waitResults[4], waitCalls, released, deleted, started;
extern "C" void port_log(const char*, ...) {}
extern "C" uint64_t sceKernelGetProcessTimeWide() { return 0; }
int sceAudioOutOpenPort(int, int, int, int) { return 7; }
int sceAudioOutSetVolume(int, int, int*) { return 0; }
int sceAudioOutReleasePort(int) { ++released; return 0; }
int sceAudioOutOutput(int, const void*) { return 0; }
int sceKernelGetThreadCurrentPriority() { return 0; }
int sceKernelChangeThreadPriority(int, int) { return 0; }
int sceKernelChangeThreadCpuAffinityMask(int, int) { return 0; }
int sceKernelGetThreadId() { return 1; }
int sceKernelGetThreadCpuAffinityMask(int) { return 0; }
int sceKernelCreateThread(const char*, int (*)(SceSize, void*), int, unsigned, unsigned, unsigned, void*) { return 5; }
int sceKernelStartThread(int, unsigned, void*) { ++started; return 0; }
int sceKernelWaitThreadEnd(int, int*, void* timeout) { assert(timeout); return waitResults[waitCalls++]; }
int sceKernelDeleteThread(int) { ++deleted; return 0; }
int sceKernelDelayThread(unsigned) { return 0; }
static void Drain(void*, int32_t) {}

int main() {
    const int timeout = static_cast<int>(0x80028005u);
    // A thread that ends in time: waited for once, deleted, port released.
    assert(port_vita_audio_open(Drain) == 1 && started == 1);
    waitResults[0] = 0;
    port_vita_audio_close();
    assert(waitCalls == 1 && deleted == 1 && released == 1 && thread == -1 && sAudioPort == -1);

    // A thread stuck in the driver: the port is released under it, and if it still does not end it is let go.
    waitCalls = deleted = released = 0;
    assert(port_vita_audio_open(Drain) == 1);
    const unsigned before = sThreadGeneration.load();
    waitResults[0] = timeout; waitResults[1] = timeout;
    port_vita_audio_close();
    assert(waitCalls == 2 && released == 1 && deleted == 0 && thread == -1 && sAudioPort == -1);
    assert(sThreadGeneration.load() == before + 1);   // the stuck thread would stop by itself when freed
    // ... a thread that wakes when the port is released ends within the second wait.
    waitCalls = deleted = released = 0;
    assert(port_vita_audio_open(Drain) == 1);
    waitResults[0] = timeout; waitResults[1] = 0;
    port_vita_audio_close();
    assert(waitCalls == 2 && released == 1 && deleted == 1 && thread == -1);

    // A resume report is counted.
    assert(port_resume_generation() == 0);
    port_resume_note(); port_resume_note();
    assert(port_resume_generation() == 2);
    std::puts("PASS: audio close is bounded when the output thread is stuck; resume reports are counted");
}
