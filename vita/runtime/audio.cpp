#include "audio.h"
#include "port/settings_lifecycle.h"

#include <atomic>
#include <stdlib.h>
#include <string.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

extern "C" void port_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace {
const int kFrames = 512;
// sceAudioOutOutput blocks only until the port has taken the PREVIOUS buffer; the buffer it is given is read while it
// plays, after the call has returned (the same contract as the PSP's sceAudioOutputBlocking, which is why pspaudiolib,
// SDL's Vita/PSP back ends and Sony's samples all alternate buffers).  Refilling one buffer in place right after the call
// overwrote audio that was still being played: a garble on top of everything at the 10.7 ms period.  Three persistent
// buffers in rotation leave each one untouched for two whole periods.
const int kOutputBuffers = 3;
alignas(64) int16_t sOutput[kOutputBuffers][kFrames * 2];
int sAudioPort = -1;
SceUID thread = -1;
PortVitaAudioDrain drainAudio;
std::atomic<bool> running{false}, failed{false};
std::atomic<int> peak{0};       // loudest sample sent to the device since port_vita_audio_take_peak
std::atomic<unsigned> outputs{0}, outputErrors{0};
port::settings::HoldGate sHold;
alignas(64) const int16_t sSilence[kFrames * 2] = {};

int audioThread(SceSize, void*) {
    unsigned next = 0;
    unsigned heldGeneration = 0, silentOutputs = 0;
    // GXV_AUDIO_BUFFERS=1: one buffer refilled in place, as before (for comparing by ear on the console).
    const char* nb = getenv("GXV_AUDIO_BUFFERS");
    const unsigned buffers = (nb && nb[0] == '1') ? 1u : (unsigned)kOutputBuffers;
    if (buffers == 1u) port_log("audio: one output buffer refilled in place (GXV_AUDIO_BUFFERS=1)");
    while (running.load(std::memory_order_acquire)) {
        const unsigned hold = sHold.Requested();
        if (hold != heldGeneration) { heldGeneration = hold; silentOutputs = 0; }
        const int16_t* samples = sSilence;
        if (!hold) {
            samples = sOutput[next];
            drainAudio(sOutput[next], kFrames);
            next = (next + 1) % buffers;
        }
        int loud = 0;
        for (int i = 0; i < kFrames * 2; ++i) {
            const int v = samples[i] < 0 ? -samples[i] : samples[i];
            if (v > loud) loud = v;
        }
        if (loud > peak.load(std::memory_order_relaxed)) peak.store(loud, std::memory_order_relaxed);
        const int r = sceAudioOutOutput(sAudioPort, samples);
        outputs.fetch_add(1, std::memory_order_relaxed);
        if (r < 0) {
            outputErrors.fetch_add(1, std::memory_order_relaxed);
            failed.store(true, std::memory_order_release);
            running.store(false, std::memory_order_release);
        } else if (hold && ++silentOutputs >= kOutputBuffers) {
            // The immutable silence buffer never overwrites a queued audible
            // buffer, including the optional legacy single-buffer configuration.
            sHold.Acknowledge(hold);
        }
    }
    return 0;
}
}

extern "C" int port_vita_audio_is_held(void) {
    const unsigned generation = sHold.Requested();
    return sHold.Acknowledged(generation) ? 1 : 0;
}

extern "C" int port_vita_audio_hold(int hold, unsigned timeoutMs) {
    unsigned generation = sHold.Requested();
    if (!hold) { sHold.Cancel(generation); return 1; }
    if (!running.load(std::memory_order_acquire)) return thread < 0 ? 1 : 0;
    if (!generation) generation = sHold.Begin();
    if (!generation) return 0;
    const uint64_t deadline = sceKernelGetProcessTimeWide() + uint64_t(timeoutMs) * 1000;
    while (!sHold.Acknowledged(generation)) {
        if (!running.load(std::memory_order_acquire) || sceKernelGetProcessTimeWide() >= deadline) {
            sHold.Cancel(generation);
            return 0;
        }
        sceKernelDelayThread(1000);
    }
    return 1;
}

// Called by the AI clock thread (platform/src/audio/ai_dsp.cpp) on its own thread: priority 0x40 like the output thread
// above. GXV_AI_PRIO=keep leaves it where the thread library put it (for comparing).
extern "C" void port_vita_audio_clock_boost(void) {
    const char* mode = getenv("GXV_AI_PRIO");
    const int before = sceKernelGetThreadCurrentPriority();
    if (mode && !strcmp(mode, "keep")) {
        port_log("audio: AI clock thread left at priority 0x%X (GXV_AI_PRIO=keep)", static_cast<unsigned>(before));
        return;
    }
    const int r = sceKernelChangeThreadPriority(0, 0x40);
    { const char* helper = getenv("GXV_HELPER_CORES"); if (!(helper && *helper == '0')) sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x60000); }  // (off core 0)
    port_log("audio: AI clock thread priority 0x%X -> 0x40: 0x%08X (cpu mask 0x%X)", static_cast<unsigned>(before), static_cast<unsigned>(r),
             static_cast<unsigned>(sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId())));
}

extern "C" int port_vita_audio_open(PortVitaAudioDrain drain) {
    if (thread >= 0 || sAudioPort >= 0 || !drain) return 0;
    failed.store(false);
    sAudioPort = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, kFrames,
                              SCE_AUDIO_OUT_SAMPLE_RATE_48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (sAudioPort < 0) { port_log("audio: sceAudioOutOpenPort failed (0x%08X)", static_cast<unsigned>(sAudioPort)); failed.store(true); return 0; }
    // A port starts at the system's idea of a default volume; the console's own games and every other
    // port of this project give it explicitly: 0 dB (full scale) on both channels.
    {
        int volume[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
        const int vr = sceAudioOutSetVolume(sAudioPort, static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), volume);
        port_log("audio: output port %d opened (MAIN, 48 kHz, 512 frames), volume set to 0 dB: 0x%08X", sAudioPort, static_cast<unsigned>(vr));
    }
    drainAudio = drain;
    // (off the game's core 0: see port_vita_thread_class; GXV_HELPER_CORES=0 leaves it to the scheduler)
    const char* helper = getenv("GXV_HELPER_CORES");
    thread = sceKernelCreateThread("galaxy_audio", audioThread, 0x40, 16 * 1024, 0, (helper && *helper == '0') ? 0 : 0x60000, nullptr);
    if (thread < 0) {
        sceAudioOutReleasePort(sAudioPort);
        sAudioPort = -1;
        failed.store(true);
        return 0;
    }
    running.store(true, std::memory_order_release);
    if (sceKernelStartThread(thread, 0, nullptr) < 0) {
        running.store(false);
        sceKernelDeleteThread(thread);
        sceAudioOutReleasePort(sAudioPort);
        thread = sAudioPort = -1;
        failed.store(true);
        return 0;
    }
    return 1;
}

extern "C" void port_vita_audio_close(void) {
    running.store(false, std::memory_order_release);
    if (thread >= 0) {
        sceKernelWaitThreadEnd(thread, nullptr, nullptr);
        sceKernelDeleteThread(thread);
        thread = -1;
    }
    if (sAudioPort >= 0) {
        sceAudioOutOutput(sAudioPort, nullptr);
        sceAudioOutReleasePort(sAudioPort);
        sAudioPort = -1;
    }
    drainAudio = nullptr;
}

extern "C" int port_vita_audio_failed(void) { return failed.load(std::memory_order_acquire); }

// The loudest sample (0..32768) sent to the device since the last call, and the output calls so far:
// the log says whether sound is going out at all.
extern "C" int port_vita_audio_take_peak(unsigned* outputCalls, unsigned* errors) {
    if (outputCalls) *outputCalls = outputs.load(std::memory_order_relaxed);
    if (errors) *errors = outputErrors.load(std::memory_order_relaxed);
    return peak.exchange(0, std::memory_order_relaxed);
}
