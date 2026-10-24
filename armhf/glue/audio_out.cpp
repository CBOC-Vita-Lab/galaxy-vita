// The Vita audio output as the platform layer sees it (vita/runtime/audio.cpp): a thread that pulls 512 stereo
// frames at 48 kHz from the ring (aaudio_out.cpp's drainAudio) every 10.67 ms.  Here the sleep that sceAudioOutOutput
// would be is a virtual-clock sleep, so the output thread's work per game frame matches the console's.
#include <stdint.h>

#include <atomic>
#include <thread>

#include "port/port.h"
#include "port/qemu_hooks.h"

typedef void (*PortVitaAudioDrain)(void* stereoS16, int32_t frames);

namespace {
std::atomic<bool> sRunning{false};
std::thread* sThread;
}

extern "C" int port_vita_audio_open(PortVitaAudioDrain drain) {
    if (sThread || !drain) return 0;
    sRunning.store(true);
    sThread = new std::thread([drain] {
        PORT_QEMU_THREAD("audio-out");
        alignas(64) int16_t samples[512 * 2];
        while (sRunning.load()) {
            drain(samples, 512);
            port_host_sleep_ns(512ll * 1000000000ll / 48000);
        }
    });
    sThread->detach();
    return 1;
}

extern "C" void port_vita_audio_close(void) { sRunning.store(false); }
extern "C" int port_vita_audio_failed(void) { return 0; }
