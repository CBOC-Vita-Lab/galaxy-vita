#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include "../vita/runtime/audio.cpp"

static std::atomic<unsigned> drains{0}, zeroOutputs{0};
static std::atomic<bool> blockOutput{false};
static std::atomic<bool> failOutput{false};
static const auto origin = std::chrono::steady_clock::now();
extern "C" uint64_t sceKernelGetProcessTimeWide() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-origin).count();
}
extern "C" void port_log(const char*, ...) {}
int sceAudioOutOpenPort(int, int, int, int) { return 7; }
int sceAudioOutSetVolume(int, int, int*) { return 0; }
int sceAudioOutReleasePort(int) { return 0; }
int sceAudioOutOutput(int, const void* p) {
    while(blockOutput.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if(failOutput.load()) return -1;
    if (p) {
        const auto* values = static_cast<const int16_t*>(p);
        if (values[0] == 0) {
            for (unsigned i=0; i<1024; ++i) assert(values[i] == 0);
            ++zeroOutputs;
        }
    }
    return 0;
}
int sceKernelGetThreadCurrentPriority() { return 0; }
int sceKernelChangeThreadPriority(int,int) { return 0; }
int sceKernelChangeThreadCpuAffinityMask(int,int) { return 0; }
int sceKernelGetThreadId() { return 1; }
int sceKernelGetThreadCpuAffinityMask(int) { return 0; }
int sceKernelCreateThread(const char*, int (*)(SceSize,void*), int,unsigned,unsigned,unsigned,void*) { return -1; }
int sceKernelStartThread(int,unsigned,void*) { return -1; }
int sceKernelWaitThreadEnd(int,int*,void*) { return 0; }
int sceKernelDeleteThread(int) { return 0; }
int sceKernelDelayThread(unsigned us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); return 0; }
static void Drain(void* data,int32_t frames) {
    ++drains;
    for(int i=0;i<frames*2;++i) static_cast<int16_t*>(data)[i]=1234;
}
int main() {
    assert(port_vita_audio_hold(1,20)); // No output device is a safe no-op.
    sAudioPort=7; thread=1; drainAudio=Drain; running=true;
    std::thread worker([] { audioThread(0,nullptr); });
    while(drains.load()<3) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(port_vita_audio_hold(1,100));
    const auto atHold=drains.load();
    assert(zeroOutputs.load()>=3);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(drains.load()==atHold);
    assert(port_vita_audio_hold(1,100));
    assert(port_vita_audio_hold(0,100));
    while(drains.load()==atHold) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    blockOutput=true;
    assert(!port_vita_audio_hold(1,5));
    blockOutput=false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(!port_vita_audio_is_held());
    assert(drains.load()>atHold+1);
    assert(port_vita_audio_hold(1,100));
    assert(port_vita_audio_hold(0,100));
    running=false;
    worker.join();
    blockOutput=true;failOutput=true;running=true;
    std::thread failingWorker([] { audioThread(0,nullptr); });
    assert(!port_vita_audio_hold(1,5));
    blockOutput=false;
    failingWorker.join();
    assert(failed.load() && !running.load() && outputErrors.load()==1);
    assert(!port_vita_audio_hold(1,100) && !port_vita_audio_is_held());
    assert(port_vita_audio_hold(0,100));
    std::puts("PASS: actual audio loop holds ring, retires audible buffers, resumes and cancels timeout");
}
