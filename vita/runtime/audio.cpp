#include "audio.h"
#include "port/core_layout.h"
#include "port/core3_budget.h"
#include "port/settings_lifecycle.h"

#include <atomic>
#include <stdlib.h>
#include <string.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#if defined(__vita__)
#include <psp2/power.h>
#endif

extern "C" void port_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern "C" void port_log_summary(const char* fmt, ...) __attribute__((weak, format(printf, 1, 2)));
// The fourth core (port/core_layout.h; vita/runtime/memory.cpp): part 1 the output thread, 2 the DSP mixer. Weak: the host tests that
// build this file have no such function, and then nothing moves.
extern "C" int port_vita_thread_core3(int part, int planned) __attribute__((weak));
extern "C" int port_vita_core3_available(void) __attribute__((weak));  // (the fourth core, proven once; vita/runtime/memory.cpp)
extern "C" void port_thread_register(const char* name) __attribute__((weak));  // (the "cores-5s:" lines' thread names, vita/runtime/memory.cpp)

namespace {
const int kFrames = 512;
// sceAudioOutOutput blocks only until the port has taken the PREVIOUS buffer; the buffer it is given is read while it
// plays, after the call has returned (the same contract as the PSP's sceAudioOutputBlocking, which is why pspaudiolib,
// SDL's Vita/PSP back ends and Sony's samples all alternate buffers).  Refilling one buffer in place right after the call
// overwrote audio that was still being played: a garble on top of everything at the 10.7 ms period.  Three persistent
// buffers in rotation leave each one untouched for two whole periods.
const int kOutputBuffers = 3;
alignas(64) int16_t sOutput[kOutputBuffers][kFrames * 2];
// The audio output and the AI clock stay off core 0 (GXV_HELPER_CORES=0 leaves them to the scheduler, GXV_CORE0_ALONE=1 overrides that).
int HelperMask() { return port::cores::HelperMask(port::cores::ReadSwitches([](const char* name) { return getenv(name); })); }
// (the AI clock is a clock-class thread: GXV_CLOCK_CORE, port/core_layout.h)
int ClockMask() { return port::cores::ClockMask(port::cores::ReadSwitches([](const char* name) { return getenv(name); })); }
int sAudioPort = -1;
SceUID thread = -1;
PortVitaAudioDrain drainAudio;
std::atomic<bool> running{false}, failed{false};
// Output threads are numbered; one that outlives its close (blocked in the driver, see port_vita_audio_close) stops by itself
// when it is let go instead of draining into the next thread's port.
std::atomic<unsigned> sThreadGeneration{0};
std::atomic<int> peak{0};       // loudest sample sent to the device since port_vita_audio_take_peak
std::atomic<unsigned> outputs{0}, outputErrors{0};
port::settings::HoldGate sHold;
alignas(64) const int16_t sSilence[kFrames * 2] = {};

// Resume reports.  The system tells a running application that the console woke up from sleep (or came back from the Home
// menu) through a power callback; the callback only counts it.  The disc reader and the audio output read the count
// (port/resume.h) and open their files and the output port afresh.  The watcher is started unless GXV_RESUME_RESET=0 (read
// once; on by default since 3.0, issue 7): the recovery from errors alone did not bring the music back (platform/src/dvd/dvd.cpp, platform/src/audio/aaudio_out.cpp).
std::atomic<unsigned> sResumeGeneration{0};
std::atomic<unsigned> sAppResumeGeneration{0};   // fix33 (A2): the application came back (PS button) without a system resume report
std::atomic<unsigned> sPendingPowerFlags{0};     // keepawake33: callback records flags atomically; powerThread logs outside callback
#if defined(__vita__)
int powerCallback(int, int, int arg, void*) {
    // fix33 (A2): a system resume (the console slept) resets the disc reader and the audio port at once; a plain PS-button return
    // (APP_RESUME alone) only opens the resume window: the reset is made if something then fails (no audio gap when nothing did).
    // keepawake33: lock-free atomic record only inside Sony's ScePower callback; powerThread logs via port_log_summary outside it.
    const unsigned flags = static_cast<unsigned>(arg);
    const unsigned suspended = SCE_POWER_CB_APP_SUSPEND | SCE_POWER_CB_SYSTEM_SUSPEND | SCE_POWER_CB_THERMAL_SUSPEND | SCE_POWER_CB_LOW_BATTERY_SUSPEND;
    const unsigned slept = SCE_POWER_CB_SYSTEM_RESUME | SCE_POWER_CB_AFTER_SYSTEM_RESUME;
    const unsigned interesting = suspended | slept | SCE_POWER_CB_SYSTEM_RESUMING | SCE_POWER_CB_APP_RESUME;
    if (flags & slept) sResumeGeneration.fetch_add(1, std::memory_order_acq_rel);
    else if (flags & SCE_POWER_CB_APP_RESUME) sAppResumeGeneration.fetch_add(1, std::memory_order_acq_rel);
    if (flags & interesting) sPendingPowerFlags.fetch_or(flags, std::memory_order_release);
    return 0;
}
int powerThread(SceSize, void*) {
    const int callback = sceKernelCreateCallback("galaxy_power", 0, powerCallback, nullptr);
    const int registered = callback >= 0 ? scePowerRegisterCallback(callback) : callback;
    if (registered < 0 && callback >= 0) sceKernelDeleteCallback(callback);
    port_log("audio: resume watcher: callback 0x%08X, registered 0x%08X", static_cast<unsigned>(callback), static_cast<unsigned>(registered));
    if (registered < 0) return 0;
    const unsigned suspended = SCE_POWER_CB_APP_SUSPEND | SCE_POWER_CB_SYSTEM_SUSPEND | SCE_POWER_CB_THERMAL_SUSPEND | SCE_POWER_CB_LOW_BATTERY_SUSPEND;
    const unsigned slept = SCE_POWER_CB_SYSTEM_RESUME | SCE_POWER_CB_AFTER_SYSTEM_RESUME | SCE_POWER_CB_SYSTEM_RESUMING;
    for (;;) {
        sceKernelDelayThreadCB(500000);  // (callbacks run inside this wait)
        const unsigned pf = sPendingPowerFlags.exchange(0, std::memory_order_acq_rel);
        if (!pf) continue;
        auto logPow = [](const char* kind, unsigned bits) {
            if (port_log_summary) port_log_summary("power: %s callback (0x%08X)", kind, bits);
            else port_log("power: %s callback (0x%08X)", kind, bits);
        };
        if (pf & suspended) logPow("suspend", pf);
        if (pf & slept) logPow("system resume", pf);
        else if (pf & SCE_POWER_CB_APP_RESUME) logPow("app resume", pf);
    }
    return 0;
}
void startResumeWatcher() {
    static bool started = false;
    const char* e = getenv("GXV_RESUME_RESET");
    if (started || (e && e[0] == '0')) return;  // (default on since 3.0)
    started = true;
    const SceUID watcher = sceKernelCreateThread("galaxy_power", powerThread, 0xA0, 16 * 1024, 0, 0, nullptr); // (16 KiB: port_log formats into a 1100-byte line on this stack, plus the C library's vfprintf frame)
    if (watcher >= 0) sceKernelStartThread(watcher, 0, nullptr);
}
#else
void startResumeWatcher() {}
#endif

int audioThread(SceSize, void*) {
    // GXV_CORE3_AUDIO=1 or 2 (2 is the default when GXV_CORE3 is not set) with a proven fourth core: this thread runs there; refused or
    // not honoured, it goes back to the helper cores it was made on. It shares no lock with the game's threads (the ring it drains is
    // lock-free), so a late slice on the system core can only cost a device underrun (counted in the audio line), not a game stall.
    const bool outOnCore3 = port_vita_thread_core3 && port_vita_thread_core3(1, HelperMask()) == 0x80000;
    // core3-38: the meter slot (the busy time of the drain: the mix of one buffer), and GXV_CORE3_AUDIO_PRIO (default 0 = off): lower than 0x40 while on the system core.
    const int meterSlot = port::core3::RegisterThread("galaxy_audio");
    port::core3::MarkOnCore3(meterSlot, outOnCore3);
    if (const int want = port::core3::AudioPriority(port::core3::ThePolicy(), false, outOnCore3, sceKernelGetThreadCurrentPriority())) {
        const unsigned before = static_cast<unsigned>(sceKernelGetThreadCurrentPriority());
        const int rc = sceKernelChangeThreadPriority(0, want);
        port_log("audio: output thread on core 3 priority 0x%X -> 0x%X (GXV_CORE3_AUDIO_PRIO): 0x%08X", before, static_cast<unsigned>(want), static_cast<unsigned>(rc));
    }
    port::core3::Worker meter(meterSlot);
    unsigned next = 0;
    unsigned heldGeneration = 0, silentOutputs = 0;
    const unsigned myGeneration = sThreadGeneration.load(std::memory_order_acquire);
    // GXV_AUDIO_BUFFERS=1: one buffer refilled in place, as before (for comparing by ear on the console).
    const char* nb = getenv("GXV_AUDIO_BUFFERS");
    const unsigned buffers = (nb && nb[0] == '1') ? 1u : (unsigned)kOutputBuffers;
    if (buffers == 1u) port_log("audio: one output buffer refilled in place (GXV_AUDIO_BUFFERS=1)");
    while (running.load(std::memory_order_acquire) && sThreadGeneration.load(std::memory_order_acquire) == myGeneration) {
        const unsigned hold = sHold.Requested();
        if (hold != heldGeneration) { heldGeneration = hold; silentOutputs = 0; }
        const int16_t* samples = sSilence;
        if (!hold) {
            samples = sOutput[next];
            meter.Wake();
            drainAudio(sOutput[next], kFrames);
            meter.Sleep();
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

extern "C" unsigned port_resume_generation(void) { return sResumeGeneration.load(std::memory_order_acquire); }
extern "C" unsigned port_resume_app_generation(void) { return sAppResumeGeneration.load(std::memory_order_acquire); }
// (what the power callback does; also how a host test reports a resume)
extern "C" void port_resume_note(void) { sResumeGeneration.fetch_add(1, std::memory_order_acq_rel); }

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
    { const int helperMask = ClockMask(); if (helperMask) sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), helperMask); }  // (off core 0; GXV_CORE0_ALONE overrides GXV_HELPER_CORES=0)
    port_log("audio: AI clock thread priority 0x%X -> 0x40: 0x%08X (cpu mask 0x%X)", static_cast<unsigned>(before), static_cast<unsigned>(r),
             static_cast<unsigned>(sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId())));
}

// Called once by the DSP mixer thread (platform/src/audio/ai_dsp.cpp) on its own thread; only exists to be called with GXV_AUDIO_THREAD=1.
// Priority 0x50, the clocks' class: a sub-frame is a short burst (about 0.1 ms) and the game's audio thread is waiting for each one, so it
// has to run when it is handed one, ahead of the renderer threads on its core.  GXV_AUDIO_THREAD_CORE (read once): 0 (default) cores 1 and 2
// (the scheduler chooses), 1 or 2 that core alone, 3 the system core (SCE_KERNEL_CPU_MASK_SYSTEM; if the kernel refuses it the thread
// takes cores 1 and 2). Core 0 is the game's and is never used.
extern "C" void port_vita_audio_mixer_boost(void) {
    static const long wanted = [] {
        const char* e = getenv("GXV_AUDIO_THREAD_CORE");
        return e && *e ? strtol(e, nullptr, 0) : 0L;
    }();
    const int before = sceKernelGetThreadCurrentPriority();
    const int pr = sceKernelChangeThreadPriority(0, 0x50);
    const int kBoth = 0x60000;  // SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2
    const int mask = wanted == 1 ? 0x20000 : wanted == 2 ? 0x40000 : wanted == 3 ? 0x80000 : kBoth;
    int ar = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask);
    if (ar < 0 && mask != kBoth) ar = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), kBoth);
    // GXV_CORE3_AUDIO=1 (or GXV_CORE3=1) with a proven fourth core: the mixer moves on there from the place chosen above, and goes back to
    // that place if the system refuses it. Not the default: the mixer takes locks the game thread also takes (the job and mail mutexes, the
    // interrupt raise), and on the system core it can be preempted while it holds one; the game thread would then wait for it.
    const bool mixOnCore3 = port_vita_thread_core3 && port_vita_thread_core3(2, 0) == 0x80000;  // (0: back to the mask it has now, the one chosen above)
    // core3-38: the meter (ai_dsp.cpp wraps each job) and GXV_CORE3_AUDIO_PRIO (default 0 = off): the mixer 0x10 below the output thread's new priority.
    port::core3::MarkOnCore3(port::core3::RegisterThread("dsp-mixer"), mixOnCore3);
    if (const int want = port::core3::AudioPriority(port::core3::ThePolicy(), true, mixOnCore3, sceKernelGetThreadCurrentPriority()))
        port_log("audio: DSP mixer on core 3 priority -> 0x%X (GXV_CORE3_AUDIO_PRIO): 0x%08X", static_cast<unsigned>(want), static_cast<unsigned>(sceKernelChangeThreadPriority(0, want)));
    port_log("audio: DSP mixer thread priority 0x%X -> 0x50: 0x%08X, cpu mask 0x%X requested -> 0x%08X (now 0x%X)", static_cast<unsigned>(before),
             static_cast<unsigned>(pr), static_cast<unsigned>(mask), static_cast<unsigned>(ar),
             static_cast<unsigned>(sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId())));
}

// GXV_AUDIO_CPU1=1 (platform/src/os/os_thread.cpp): called once by the game's audio thread (JASAudioThread) on its own host thread.  It
// may now run on a second virtual CPU while the game thread works, so it leaves core 0: the core GXV_AUDIO_THREAD_CORE gives the DSP mixer
// (default 1, the render thread's, which has room; 0: cores 1-2), at priority 0x4F, just above the mixer (0x50): a run (one wake-up's
// work) is not cut by the mix of the sub-frame it has just released, so it ends sooner (the game thread may be waiting for it to enter
// a critical section) and the mix follows.  Above the renderer threads.  Its runs on the game's CPU (when the game thread sleeps, or
// outside the windows) run there too: the game thread then waits for it either way.
//
// GXV_AUDIO_CPU1_CORE (sched24, read once, default 0 = follow GXV_AUDIO_THREAD_CORE as above): 1 or 2 that core alone; 3 the fourth core, only when it
// is proven (port_vita_core3_available) and the system keeps the thread there (it is put back on core 1 otherwise), where the DSP mixer and the output
// thread already are by default: this thread hands each sub-frame to the mixer and waits for it, so on the same core the hand-over needs no
// cross-core wake-up; the price is a core that is not always ours (the 5 s "audio cpu:" line shows the game thread's waits for it). GXV_AUDIO_PRIO
// (sched24, 0x40-0xBF, default 0x4F) is its priority.
extern "C" int port_vita_audio_cpu1_boost(int* priority) {
    static const long wanted = [] {
        const char* e = getenv("GXV_AUDIO_THREAD_CORE");
        return e && *e ? strtol(e, nullptr, 0) : 0L;
    }();
    static const long own = [] {
        const char* e = getenv("GXV_AUDIO_CPU1_CORE");
        return e && *e ? strtol(e, nullptr, 0) : 0L;
    }();
    static const long prio = [] {
        const char* e = getenv("GXV_AUDIO_PRIO");
        const long v = e && *e ? strtol(e, nullptr, 0) : 0x4F;
        return v >= 0x40 && v <= 0xBF ? v : 0x4F;
    }();
    sceKernelChangeThreadPriority(0, static_cast<int>(prio));
    if (port_thread_register) port_thread_register("jas-audio");  // (this host thread is the game's audio thread: it was named "guest" at its start)
    const int kBoth = 0x60000;  // SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2
    const long core = own >= 1 && own <= 3 ? own : wanted == 3 ? 0L : wanted;  // (GXV_AUDIO_THREAD_CORE=3 is the mixer's: this thread keeps its old plan)
    int mask = core == 1 ? 0x20000 : core == 2 ? 0x40000 : kBoth;
    int ar;
    if (core == 3 && port_vita_core3_available && port_vita_core3_available()) {
        // (the fourth core: asked for on the thread itself and kept only when the mask reads back as asked, as PlaceOnSystemCore does)
        ar = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x80000);
        if (ar >= 0 && sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId()) == 0x80000) mask = 0x80000;
        else { mask = 0x20000; ar = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask); }
    } else {
        if (core == 3) mask = 0x20000;  // (not proven: core 1, the render thread's)
        ar = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask);
    }
    if (ar < 0 && mask != kBoth) ar = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), kBoth);
    // (the caller writes the basic log's line; port_log would not reach log.txt in a build without runtime diagnostics)
    if (priority) *priority = sceKernelGetThreadCurrentPriority();
    return ar < 0 ? ar : sceKernelGetThreadCpuAffinityMask(sceKernelGetThreadId());
}

extern "C" int port_vita_audio_open(PortVitaAudioDrain drain) {
    if (thread >= 0 || sAudioPort >= 0 || !drain) return 0;
    startResumeWatcher();
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
    thread = sceKernelCreateThread("galaxy_audio", audioThread, 0x40, 16 * 1024, 0, HelperMask(), nullptr);
    if (thread < 0) {
        sceAudioOutReleasePort(sAudioPort);
        sAudioPort = -1;
        failed.store(true);
        return 0;
    }
    sThreadGeneration.fetch_add(1, std::memory_order_acq_rel);
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

// How long the output thread gets to leave its loop once told to.  It is in sceAudioOutOutput for at most one period (10.7 ms)
// when the port works; after the console woke up from sleep the call has been seen to return an error, and a port the system
// no longer serves could just as well never return.  Waiting for ever there froze the audio clock (the music never came back).
// After the first wait the port is released, which wakes a thread blocked on it; after the second the thread is let go.
constexpr unsigned kCloseWaitUs = 1000000;
extern "C" void port_vita_audio_close(void) {
    running.store(false, std::memory_order_release);
    if (thread >= 0) {
        unsigned timeoutUs = kCloseWaitUs;
        int r = sceKernelWaitThreadEnd(thread, nullptr, &timeoutUs);
        if (r == static_cast<int>(0x80028005u) /* SCE_KERNEL_ERROR_WAIT_TIMEOUT */) {
            port_log("audio: the output thread did not stop in %u ms; releasing the port under it", kCloseWaitUs / 1000);
            if (sAudioPort >= 0) { sceAudioOutReleasePort(sAudioPort); sAudioPort = -1; }
            timeoutUs = kCloseWaitUs;
            r = sceKernelWaitThreadEnd(thread, nullptr, &timeoutUs);
            if (r == static_cast<int>(0x80028005u)) {
                port_log("audio: the output thread is stuck; letting it go");
                sThreadGeneration.fetch_add(1, std::memory_order_acq_rel);  // (it ends as soon as the driver lets it)
                thread = -1;  // (its stack is kept; deleting a running thread fails)
            }
        }
        if (thread >= 0) {
            sceKernelDeleteThread(thread);
            thread = -1;
        }
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
