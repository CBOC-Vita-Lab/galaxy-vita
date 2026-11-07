// Audio interface (AI) and the DSP mailbox, emulated at the level JAudio2's
// own driver (dsptask.cpp / osdsp_task.cpp) talks to.
//
// The DSP side implements the JAudio microcode's mailbox protocol:
//   boot            -> mails 0xDCD10000, <any>      (DspHandShake)
//   command packet  -> [count][word0..]             (DSPSendCommands2)
//   0x82 syncFrame  -> N sub-frames to render, each when the game releases the voices
//   release halt    -> with the last voices released: one sub-frame mixed, then 0xDCD10004,0xF355FF00
//   0x81/0x8E etc.  -> 0xDCD10004, 0xF355<word0 >> 16> (DspFinishWork)
// Mixing is delegated to port_dsp_mix_frame (silence until the mixer lands).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "port/heap_routing.h"
#include "port/port.h"
#include "port/diagnostics.h"
#include "port/qemu_hooks.h"
#include "../../vgx/src/vgx_workload.h"
#include "revolution/ai.h"
#include "revolution/dsp.h"
#include "revolution/os.h"

// ---------------------------------------------------------------------------
// DSP mailbox
// ---------------------------------------------------------------------------
extern "C" {
DSPTaskInfo* __DSP_first_task;
DSPTaskInfo* __DSP_curr_task;
DSPTaskInfo* __DSP_last_task;
DSPTaskInfo* __DSP_tmp_task;
void __DSPHandler(__OSInterrupt interrupt, OSContext* context);
}

static std::mutex sMailMutex;
static std::deque<u32> sMailFromDsp;

static std::vector<u32> sPacket;
static u32 sPacketExpected = 0;
static bool sWaitingCount = true;
static u32 sVaramBase = 0;
static u16 sMixerLevel = 0x4000;

extern "C" u32 port_dsp_varam_base(void) { return sVaramBase; }

static void queueMail(u32 a, u32 b) {
    // Runs on game threads (DSPSendMailToDSP -> the HLE): the deque's 512-byte nodes are host memory.  Without the scope
    // they land in whichever JKR heap is current, which is the (full) root GDDR heap right after a stage change
    // (found by the armhf harness: "heap error: heap root GDDR3 size 512 alignment 4" entering Gateway Galaxy).
    PortHostAllocScope host;
    std::lock_guard<std::mutex> lk(sMailMutex);
    sMailFromDsp.push_back(a);
    sMailFromDsp.push_back(b);
}

// Mixer setup (command 0x81) and the software mixer's entry point; implemented in dsp_mixer.cpp.
// Builds that link the object files directly may do without the mixer, so they get silent weak
// stubs. The console build links a static archive, where a weak definition would satisfy the
// reference and the linker would never pull dsp_mixer.o in: there the mixer is a plain
// reference (the console played no sound at all until this was found).
#if defined(__vita__)
extern "C" void port_dsp_setup(u32 voiceCount, u32 voices, u32 table, u32 afcTable, u32 fxBlocks);
extern "C" void port_dsp_mix_frame(u32 subFrames, s16* outL, s16* outR, u16 mixerLevel);
#else
extern "C" void __attribute__((weak)) port_dsp_setup(u32 voiceCount, u32 voices, u32 table, u32 afcTable, u32 fxBlocks) {
    (void)voiceCount, (void)voices, (void)table, (void)afcTable, (void)fxBlocks;
}

extern "C" void __attribute__((weak)) port_dsp_mix_frame(u32 subFrames, s16* outL, s16* outR, u16 mixerLevel) {
    (void)mixerLevel;
    memset(outL, 0, subFrames * 80 * sizeof(s16));
    memset(outR, 0, subFrames * 80 * sizeof(s16));
}
#endif

// The sync frame being rendered.  As on the console it goes one sub-frame at
// a time: the game updates the voices (JASDSPChannel::updateAll), releasing
// them to the DSP sixteen at a time; with the last sixteen released the DSP
// renders the sub-frame's 80 samples and reports with a mail, on which the
// game updates the voices for the next one.  The game so follows each voice
// 80 samples at a time, and JASAramStream counts on it: it sets up a stream's
// loop (or its end) once the voice is within 400 samples of it, and takes a
// voice found beyond it for a fatal error that silences every stream until
// the game is restarted.  Rendered a whole frame at once, a voice moved 560
// samples between two looks and jumped that window on about one pass in
// four: the music of the file select screen stopped for good after 55 s.
static struct {
    u32 subFrames = 0;
    u32 done = 0;
    s16* outL = nullptr;
    s16* outR = nullptr;
} sFrame;

// Time spent in the software mixer (80-sample sub-frames), for the audio log line.
static std::atomic<int64_t> sMixNs{0}, sMixMaxNs{0};
static std::atomic<u32> sMixCalls{0};

// The mixer renders one 80-sample sub-frame into the game's two output buffers.
struct MixJob {
    s16* outL;
    s16* outR;
    u16 level;
};

static void mixOne(const MixJob& job) {
    const int64_t t0 = PORT_RUNTIME_DIAGNOSTICS ? port_host_time_ns() : 0;
    {
        vgx::workload::Scope observation(vgx::workload::Dsp);
        port_dsp_mix_frame(1, job.outL, job.outR, job.level);
    }
    const int64_t dt = PORT_RUNTIME_DIAGNOSTICS ? port_host_time_ns() - t0 : 0;
    sMixNs.fetch_add(dt, std::memory_order_relaxed);
    sMixCalls.fetch_add(1, std::memory_order_relaxed);
    if (dt > sMixMaxNs.load(std::memory_order_relaxed)) sMixMaxNs.store(dt, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The mixing on a thread of its own (GXV_AUDIO_THREAD=1)
//
// On the Wii the sub-frame is rendered by the DSP chip, a processor of its own: the CPU's audio thread releases the voices
// (the 0x00 packet of the last group of sixteen), goes back to waiting for a message, and the DSP's mail wakes it for the next
// sub-frame.  Here the mixer is software and ran inside the release, on the guest CPU, i.e. on core 0 in front of the game's
// own work (about 0.1 ms a sub-frame, 400 a second).  With the switch on, the release hands the sub-frame to a host thread
// (cores 1-2 on the console, see port_vita_audio_mixer_boost) and returns; the guest's audio thread then blocks in its message
// queue, the game thread runs on, and when the thread has finished it queues the same reply mail and raises the same DSP
// interrupt, which the guest takes at its next safe point.  What the game's audio code (JASAudioThread, JASDriver, the
// sequences, JASPortCmd and its queues) does is untouched: it still runs on the guest CPU, in the same order, and the
// protocol stays lock-step (one sub-frame in flight; the next release only comes after the reply mail).  Every other command
// waits for the sub-frame in flight to be finished and its mail queued first, so the DSP's in-order mailbox is kept.
// Off (the default) the sub-frame is rendered inside the release as before.
//   GXV_AUDIO_THREAD=0   mix inside the release (default)
//   GXV_AUDIO_THREAD=1   mix on the mixer thread; if the thread cannot be made, inside the release
// The sub-frames' PCM and the voice blocks it leaves are the same bytes either way (tests/dsp_async_mix.cpp).
// ---------------------------------------------------------------------------
extern "C" void __attribute__((weak)) port_vita_audio_mixer_boost(void);  // vita/runtime/audio.cpp: core and priority

static bool mixThreadWanted() {
    static const bool on = vgx::EnvInt("GXV_AUDIO_THREAD", 0) == 1;  // read once; default off
    return on;
}

// (Never destroyed: the mixer thread is detached and still waits on them when the process exits.)
static std::mutex& sJobMutex = *new std::mutex;
static std::condition_variable& sJobCv = *new std::condition_variable;   // the mixer thread waits here for a job
static std::condition_variable& sDoneCv = *new std::condition_variable;  // the guest waits here for the job in flight
static MixJob sJob;
static bool sJobPending = false;           // guarded by sJobMutex
static std::atomic<bool> sJobBusy{false};  // handed over, and its reply mail is not queued yet
static std::atomic<int> sMixThreadState{0};  // 0: not started, 1: running, -1: could not be made
static std::atomic<u32> sMixThreadJobs{0}, sMixDrainWaits{0};
static std::atomic<int64_t> sMixDrainNs{0};

static void* mixThreadMain(void*) {
    PORT_QEMU_THREAD("dsp-mixer");
    if (port_vita_audio_mixer_boost) {
        port_vita_audio_mixer_boost();
    }
    for (;;) {
        MixJob job;
        {
            std::unique_lock<std::mutex> lk(sJobMutex);
            sJobCv.wait(lk, [] { return sJobPending; });
            job = sJob;
            sJobPending = false;
        }
        mixOne(job);
        sMixThreadJobs.fetch_add(1, std::memory_order_relaxed);
        // The mail first, then the "free" flag: whoever sees the flag clear queues its own mail behind this one.
        queueMail(0xDCD10004, 0xF355FF00);
        {
            std::lock_guard<std::mutex> lk(sJobMutex);
            sJobBusy.store(false, std::memory_order_release);
        }
        sDoneCv.notify_all();
        port_irq_raise(PORT_IRQ_DSP);
    }
    return nullptr;
}

// Guest side: returns when no sub-frame is in flight (its voice blocks and PCM written, its mail queued).
static void drainMix() {
    if (!sJobBusy.load(std::memory_order_acquire)) {
        return;
    }
    const int64_t t0 = PORT_RUNTIME_DIAGNOSTICS ? port_host_time_ns() : 0;
    {
        std::unique_lock<std::mutex> lk(sJobMutex);
        sDoneCv.wait(lk, [] { return !sJobBusy.load(std::memory_order_acquire); });
    }
    sMixDrainWaits.fetch_add(1, std::memory_order_relaxed);
    if (PORT_RUNTIME_DIAGNOSTICS) sMixDrainNs.fetch_add(port_host_time_ns() - t0, std::memory_order_relaxed);
}

static bool startMixThread() {
    const int state = sMixThreadState.load(std::memory_order_acquire);
    if (state != 0) {
        return state > 0;
    }
    PortHostAllocScope host;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128u << 10);
    pthread_t thread;
    const int rc = pthread_create(&thread, &attr, mixThreadMain, nullptr);
    pthread_attr_destroy(&attr);
    if (rc == 0) {
        pthread_detach(thread);
    }
    port_log("audio: DSP mixer %s (GXV_AUDIO_THREAD=1)", rc == 0 ? "on a thread of its own" : "thread could not be made; mixing in the release");
    sMixThreadState.store(rc == 0 ? 1 : -1, std::memory_order_release);
    return rc == 0;
}

// Hands one sub-frame to the mixer thread; false: the caller mixes it itself.
static bool submitMix(const MixJob& job) {
    if (!startMixThread()) {
        return false;
    }
    drainMix();  // (lock-step: nothing is in flight here unless the game broke the protocol)
    {
        std::lock_guard<std::mutex> lk(sJobMutex);
        sJob = job;
        sJobPending = true;
        sJobBusy.store(true, std::memory_order_release);
    }
    sJobCv.notify_one();
    return true;
}

// (tests/dsp_async_mix.cpp) Sub-frames the mixer thread has rendered so far.
extern "C" unsigned port_dsp_mixer_thread_jobs(void) { return sMixThreadJobs.load(std::memory_order_relaxed); }

// GXV_AUDIO_HOLD (platform/src/os/os_thread.cpp): while a sync frame has sub-frames left to render, its DSP interrupts continue it
// and the game thread takes them at once; the mail of the frame's last sub-frame and the AI interrupt may wait for the game thread's
// next sleep. Weak: the host tests that link this file without the scheduler (tests/dsp_async_mix.cpp) have neither function.
extern "C" void __attribute__((weak)) port_audio_irq_urgent(int urgent);
extern "C" int __attribute__((weak)) port_audio_hold_stats(unsigned* elsewhere, unsigned* expired, unsigned* atOnce, unsigned* begun);
static void noteFrameState() {
    if (port_audio_irq_urgent) port_audio_irq_urgent(sFrame.done < sFrame.subFrames ? 1 : 0);
}

static void executePacket() {
    drainMix();  // (GXV_AUDIO_THREAD) the DSP does its commands in order: a sub-frame in flight is finished, its mail queued, first
    const std::vector<u32>& p = sPacket;
    u32 w0 = p.empty() ? 0 : p[0];
    u32 cmd = w0 >> 24;
    switch (cmd) {
    case 0x82:  // sync frame: `subFrames` sub-frames of 80 samples to render
        sFrame.subFrames = (w0 >> 16) & 0xFF;
        sFrame.done = 0;
        sMixerLevel = (u16)(w0 & 0xFFFF);
        sFrame.outL = (s16*)(uintptr_t)(p.size() > 1 ? p[1] : 0);
        sFrame.outR = (s16*)(uintptr_t)(p.size() > 2 ? p[2] : 0);
        break;
    case 0x00:  // release halt: voices 0 .. 16 * (group + 1) - 1 are ready for the sub-frame
        if (((w0 >> 16) & 0xFF) == 3 && sFrame.done < sFrame.subFrames) {
            if (sFrame.outL && sFrame.outR) {
                const MixJob job{sFrame.outL + sFrame.done * 80, sFrame.outR + sFrame.done * 80, sMixerLevel};
                sFrame.done++;
                if (mixThreadWanted() && submitMix(job)) {
                    break;  // (the mixer thread queues the reply mail and raises the interrupt when it is done)
                }
                mixOne(job);
            } else {
                sFrame.done++;
            }
            queueMail(0xDCD10004, 0xF355FF00);
        }
        break;
    case 0x8E:  // set VARAM base (alt-ARAM start in MEM2)
        sVaramBase = p.size() > 1 ? p[1] : 0;
        queueMail(0xDCD10004, 0xF3550000 | (w0 >> 16));
        break;
    case 0x81:  // table setup (channel buffer, filters, FX buffer)
        if (p.size() >= 5) {
            port_dsp_setup(w0 & 0xFFFF, p[1], p[2], p[3], p[4]);
        }
        queueMail(0xDCD10004, 0xF3550000 | (w0 >> 16));
        break;
    default:
        // Other control words need no reply.
        break;
    }
    noteFrameState();  // (GXV_AUDIO_HOLD; before the guest CPU can go back to the game thread)
    port_irq_raise(PORT_IRQ_DSP);
}

static void dspIrq() {
    // Each __DSPHandler call consumes one event (a DCD1 mail and, for requests,
    // the payload mail read by the task's callback).
    for (int guard = 0; guard < 256; guard++) {
        {
            std::lock_guard<std::mutex> lk(sMailMutex);
            if (sMailFromDsp.empty()) {
                break;
            }
        }
        __DSPHandler(__OS_INTERRUPT_DSP_DSP, OSGetCurrentContext());
    }
}

extern "C" {

void DSPInit(void) {
    port_irq_set_handler(PORT_IRQ_DSP, dspIrq);
}

u32 DSPCheckMailToDSP(void) { return 0; }  // the HLE consumes mail instantly

u32 DSPCheckMailFromDSP(void) {
    std::lock_guard<std::mutex> lk(sMailMutex);
    return sMailFromDsp.empty() ? 0 : 0x80000000u;
}

u32 DSPReadMailFromDSP(void) {
    std::lock_guard<std::mutex> lk(sMailMutex);
    if (sMailFromDsp.empty()) {
        return 0;
    }
    u32 m = sMailFromDsp.front();
    sMailFromDsp.pop_front();
    return m;
}

void DSPSendMailToDSP(u32 mail) {
    PortHostAllocScope host;  // (sPacket may grow; host memory, see queueMail)
    if (sWaitingCount) {
        sPacket.clear();
        sPacketExpected = mail == 0 ? 1 : mail;
        sWaitingCount = false;
        return;
    }
    sPacket.push_back(mail);
    if (sPacket.size() >= sPacketExpected) {
        sWaitingCount = true;
        executePacket();
    }
}

void DSPAssertInt(void) {}

void __DSP_boot_task(DSPTaskInfo* task) {
    __DSP_curr_task = task;
    // Microcode is "running": report init, then the second handshake mail.
    queueMail(0xDCD10000, 0x8071FEED);
    port_irq_raise(PORT_IRQ_DSP);
}

void __DSP_exec_task(DSPTaskInfo*, DSPTaskInfo*) {}
void __DSP_remove_task(DSPTaskInfo*) {}

}  // extern "C"

// ---------------------------------------------------------------------------
// Audio interface DMA, paced by the host audio device (see aiClock).
// ---------------------------------------------------------------------------
static AIDCallback sDmaCallback;
static std::atomic<u32> sDmaAddr{0};
static std::atomic<u32> sDmaLen{0};
static std::atomic<u32> sDmaLatched{0};  // AIInitDMA calls so far
static std::atomic<bool> sDmaRunning{false};
static std::atomic<u32> sSampleRate{32000};
static bool sAiThreadStarted;

// Host audio sink (implemented by the platform audio output): plays a block,
// reports how much audio the device has queued (< 0 without a device) and
// how often it ran dry.
extern "C" void __attribute__((weak)) port_audio_submit(const s16* interleavedLE, u32 frames, u32 rate) {
    (void)interleavedLE;
    (void)frames;
    (void)rate;
}
extern "C" int64_t __attribute__((weak)) port_audio_queued_ns(void) { return -1; }
extern "C" int __attribute__((weak)) port_audio_underruns(void) { return 0; }
// The loudest sample sent to the output device since the last call (< 0: not measured here).
extern "C" int __attribute__((weak)) port_audio_take_peak(void) { return -1; }
extern "C" int port_handheld(void);
// The console raises this thread's priority: it only sleeps and hands finished blocks to the output device, and when a busy
// movie decode keeps all cores occupied a late wake-up lets the device run dry (a click or a dropout in the sound).
extern "C" void __attribute__((weak)) port_vita_audio_clock_boost(void);
// Remote speaker stream health (aaudio_out.cpp; 0 when the emulated speaker is off).
extern "C" int __attribute__((weak)) port_audio_take_speaker_stats(unsigned* underruns, unsigned* dropped) {
    (void)underruns, (void)dropped;
    return 0;
}

// Blocks JAudio had to fill with a held sample because the DSP frame for them was not finished (JASDriver::readDspBuffer):
// a dropout the "replayed" count does not see, since the game did answer the AI interrupt in time.
static std::atomic<u32> sDspHolds{0}, sDspHolds5{0};
extern "C" void port_audio_note_dsp_hold(void) {
    sDspHolds.fetch_add(1, std::memory_order_relaxed);
    sDspHolds5.fetch_add(1, std::memory_order_relaxed);  // (the 5 s line)
}

// GXV_AUDIO_CPU1 set (0 or 1), or GXV_AUDIO_STATS=1 (platform/src/os/os_thread.cpp counts): every 5 s, where the game's audio thread ran
// and what it cost the game thread on core 0, with the sound's health over the same 5 s.  Weak: the host tests that link this file without
// the scheduler have no counts.
extern "C" int __attribute__((weak)) port_audio_cpu_stats(int64_t ns[7], unsigned n[8]);
// The basic log (ux0:data/smg/log.txt): port_log reaches it only in builds with runtime diagnostics. Weak for the host tests.
extern "C" void __attribute__((weak)) port_log_summary(const char* fmt, ...);
// (the AI clock thread, after each block; `blocks5`/`late5` are its counts since the last line, cleared here)
static void audioCpuLine(u32& blocks5, u32& late5) {
    static int64_t last = 0;
    static int lastUnderruns = 0;
    static bool off = false;
    if (off || !port_audio_cpu_stats) return;
    const int64_t now = port_host_time_ns();
    if (last == 0) {  // (the first block: the counts start here)
        int64_t ns[7];
        unsigned n[8];
        if (port_audio_cpu_stats(ns, n) < 0) {
            off = true;  // (not counting: the switches are unset)
            return;
        }
        last = now;
        lastUnderruns = port_audio_underruns();
        sDspHolds5.store(0, std::memory_order_relaxed);
        blocks5 = late5 = 0;
        return;
    }
    if (now - last < 5000000000ll) return;
    const double secs = (now - last) / 1e9;
    last = now;
    int64_t ns[7] = {};
    unsigned n[8] = {};
    const int mode = port_audio_cpu_stats(ns, n);
    const int underruns = port_audio_underruns();
    const u32 dspLate = sDspHolds5.exchange(0, std::memory_order_relaxed);
    if (mode < 0 || !port_log_summary) return;
    const double turns = secs * 60.0;  // (per 1/60 s: the game's turns at full speed)
    // "taken from the game thread": the audio thread on core 0 while the game thread was ready to run, plus the game thread's waits for a
    // run on the second CPU; that is what the switch is for.
    // (mode 2: on, but the audio thread never started; windows 0 with the switch on: GameSystem::calcAnim/draw never opened one)
    port_log_summary("audio cpu: %.1f s, GXV_AUDIO_CPU1=%d%s: taken from the game thread %.2f ms per turn (core 0: audio thread %.1f ms in %u runs "
             "while the game thread was ready, %.1f ms while it slept; waits for core 1 %.1f ms, %u, %u at a window's end); core 1: audio "
             "thread %.1f ms in %u runs (%u interrupt batches); other guest threads %.1f ms, idle %.1f ms; %u windows (%u closed by JAudio, "
             "%u gate closes); sound: %u blocks, %u late (played as silence), %u DSP late, %d device underruns",
             secs, mode == 2 ? 1 : mode,
             mode == 2 ? " (NOT IN EFFECT: the game's audio thread has not marked itself)" : mode == 1 && n[5] == 0 ? " (NOT IN EFFECT: no window opened)" : "",
             (ns[1] + ns[6]) / 1e6 / turns, ns[1] / 1e6, n[0], ns[4] / 1e6, ns[6] / 1e6, n[3], n[4], ns[5] / 1e6, n[1], n[2],
             ns[2] / 1e6, ns[3] / 1e6, n[5], n[6], n[7], blocks5, late5, dspLate, underruns - lastUnderruns);
    lastUnderruns = underruns;
    blocks5 = late5 = 0;
}

static void aiIrq() {
    if (sDmaCallback) {
        sDmaCallback();
    }
}

// The AI hardware: plays the block the game latched with AIInitDMA and
// interrupts as it starts, so the game can latch the next one (JAudio mixes
// a block per interrupt).  A new block starts whenever the device has less
// than kTarget queued, so the device's clock paces the game's audio; without
// a device, a timer does.
static void aiClock() {
    PORT_QEMU_THREAD("ai-clock");
    if (port_vita_audio_clock_boost) {
        port_vita_audio_clock_boost();
    }
    // The game answers an interrupt from its audio thread, which only gets
    // the CPU at the running thread's next safe point, sometimes 10-20 ms
    // later.  The queued audio covers that.
    // A block waits for the game to latch a new one until the device is down
    // to kLow (two of the Quest's 4 ms bursts); then the latched block plays
    // again, as on the console.
#if defined(__vita__)
    // The Vita's audio port takes 512-frame bursts (10.7 ms at 48 kHz) and the game thread is often 100-250 ms late
    // while a stage loads.  With kLow under one burst the device ran dry before a late block was answered with silence
    // (92 underruns in a bad 10 s window); two bursts of margin keeps it fed.
    int64_t kTarget = 44000000;
    const int64_t kLow = 22000000;
#else
    int64_t kTarget = 32000000;
    const int64_t kLow = 8000000;
#endif
    // PETARI_AUDIO_QUEUE_MS=<n>: queue more audio ahead of the device (more slack for a late game turn before a block has
    // to be played as silence, at the price of n - default ms more sound latency); for console comparisons.
    if (const char* q = getenv("PETARI_AUDIO_QUEUE_MS")) {
        const long ms = strtol(q, nullptr, 10);
        if (ms >= 20 && ms <= 200 && ms * 1000000ll > kLow + 10000000ll) {
            kTarget = ms * 1000000ll;
            port_log("audio: device queue target %ld ms (PETARI_AUDIO_QUEUE_MS)", ms);
        }
    }
    int64_t next = port_host_time_ns();  // start of the next block (timer pacing)
    u32 answered = sDmaLatched.load();
    u32 blocks = 0, late = 0, lateRun = 0, lateRunMax = 0;
    u32 blocks5 = 0, late5 = 0;  // (the 5 s audio cpu line)
    int64_t reportAt = next + 10000000000ll, minQueued = INT64_MAX, maxQueued = -1;
    static const bool logAlways = getenv("PETARI_AUDIOLOG") != nullptr;
    // How long this thread goes between two passes of its loop (it sleeps 4 ms at most): a long gap is the scheduler keeping
    // it from running, not the game being late.
    int64_t lastPass = next, gapMax = 0;
    u32 gapsOver = 0, gapsOverLong = 0;
    // When the game does not answer in time (a long stretch of work on the guest CPU that takes no interrupts: a stage
    // load), the console's AI would play the last block again, and again.  On the handhelds that came out as a buzz at the
    // block rate, with the music running at a fraction of its speed.  A late block is played as silence instead, the last
    // sample dying away in 2 ms, and the next real block fades in over 2 ms: a pause in the music, not a stutter.
    static s16 sHush[4096 * 2];
    s16 lastFrame[2] = {0, 0};
    bool hushed = false;
    for (;;) {
        {
            const int64_t t = PORT_RUNTIME_DIAGNOSTICS ? port_host_time_ns() : lastPass;
            const int64_t gap = t - lastPass;
            lastPass = t;
            if (gap > gapMax) gapMax = gap;
            if (gap > 10000000) gapsOver++;
            if (gap > 20000000) gapsOverLong++;
        }
        if (!sDmaRunning.load() || port_is_paused()) {
            port_host_sleep_ns(2000000);
            next = port_host_time_ns();
            continue;
        }
        u32 rate = sSampleRate.load();
        u32 frames = sDmaLen.load() / 4;
        int64_t dur = frames ? (int64_t)frames * 1000000000ll / (rate ? rate : 32000) : 5000000;
        int64_t now = port_host_time_ns();
        int64_t queued = port_audio_queued_ns();  // < 0: no device
        if (queued >= 0) {
            if (queued > kTarget) {
                int64_t wait = queued - kTarget;
                port_host_sleep_ns(wait < 4000000 ? wait : 4000000);
                continue;
            }
        } else if (now < next) {
            port_host_sleep_ns(next - now);
            continue;
        }
        bool lateBlock = false;
        if (sDmaLatched.load() == answered) {
            bool slack = queued >= 0 ? queued > kLow : now < next + dur / 2;
            if (slack) {
                port_host_sleep_ns(500000);
                continue;
            }
            late++;
            late5++;
            lateBlock = true;
            if (++lateRun > lateRunMax) lateRunMax = lateRun;
        } else {
            lateRun = 0;
        }
        answered = sDmaLatched.load();
        u32 addr = sDmaAddr.load();
        frames = sDmaLen.load() / 4;
        if (addr && frames) {
            const s16* block = (const s16*)(uintptr_t)addr;
            if (frames <= 4096 && (lateBlock || hushed)) {
                // (sample pairs are right, left)
                const u32 ramp = frames < 64 ? frames : 64;
                if (lateBlock) {
                    for (u32 i = 0; i < frames; i++) {
                        const float g = i < ramp ? 1.0f - (float)(i + 1) / (float)ramp : 0.0f;
                        sHush[i * 2] = (s16)(lastFrame[0] * g);
                        sHush[i * 2 + 1] = (s16)(lastFrame[1] * g);
                    }
                    port_audio_submit(sHush, frames, rate);
                    lastFrame[0] = lastFrame[1] = 0;
                    hushed = true;
                } else {
                    for (u32 i = 0; i < frames; i++) {
                        const float g = i < ramp ? (float)(i + 1) / (float)ramp : 1.0f;
                        sHush[i * 2] = (s16)(block[i * 2] * g);
                        sHush[i * 2 + 1] = (s16)(block[i * 2 + 1] * g);
                    }
                    port_audio_submit(sHush, frames, rate);
                    lastFrame[0] = block[(frames - 1) * 2];
                    lastFrame[1] = block[(frames - 1) * 2 + 1];
                    hushed = false;
                }
            } else {
                port_audio_submit(block, frames, rate);
                lastFrame[0] = block[(frames - 1) * 2];
                lastFrame[1] = block[(frames - 1) * 2 + 1];
            }
        }
        port_irq_raise(PORT_IRQ_AI);
        blocks++;
        blocks5++;
        audioCpuLine(blocks5, late5);  // (GXV_AUDIO_CPU1 / GXV_AUDIO_STATS: every 5 s)
        if (queued < minQueued) minQueued = queued;
        if (queued > maxQueued) maxQueued = queued;
        next += dur;
        if (now - next > 100000000) {
            next = now;  // resync after a long stall instead of bursting
        }
        if (PORT_RUNTIME_DIAGNOSTICS && now >= reportAt) {
            // PETARI_AUDIOLOG=1 reports every 10 s; otherwise only trouble.
            static int lastUnderruns = 0;
            int underruns = port_audio_underruns();
            // (a handheld says so every time: the line is how a console's sound is checked without ears)
            static const bool logEvery = logAlways || port_handheld();
            const int peak = port_audio_take_peak();
            const u32 mixes = sMixCalls.exchange(0);
            const int64_t mixNs = sMixNs.exchange(0), mixMax = sMixMaxNs.exchange(0);
            const u32 dspHolds = sDspHolds.exchange(0, std::memory_order_relaxed);
            char mt[112] = "";
            if (sMixThreadState.load(std::memory_order_acquire) > 0) {
                snprintf(mt, sizeof(mt), "; mixer thread %u sub-frames, the game's audio waited for it %u times (%.2f ms)",
                         sMixThreadJobs.exchange(0, std::memory_order_relaxed), sMixDrainWaits.exchange(0, std::memory_order_relaxed),
                         sMixDrainNs.exchange(0, std::memory_order_relaxed) / 1e6);
            }
            // (GXV_AUDIO_HOLD) how the audio interrupts were taken: "in the game's waits" is what the switch is for (taken while the game
            // thread slept, or by another guest thread), "at the limit" by the running game thread after the whole limit, "at once" by the
            // running game thread without a hold (a frame's sub-frames, a yield); "held" counts the holds begun.
            char hold[160] = "";
            unsigned hElse = 0, hExp = 0, hNow = 0, hBegun = 0;
            if (port_audio_hold_stats) {
                if (const int holdMs = port_audio_hold_stats(&hElse, &hExp, &hNow, &hBegun)) {
                    snprintf(hold, sizeof(hold), "; audio interrupts (GXV_AUDIO_HOLD=%d): %u in the game's waits, %u at the limit, %u at once, %u held",
                             holdMs, hElse, hExp, hNow, hBegun);
                }
            }
            unsigned spkUnder = 0, spkDropped = 0;
            const bool spkOn = port_audio_take_speaker_stats(&spkUnder, &spkDropped) != 0;
            if (late || dspHolds || underruns != lastUnderruns || logEvery) {
                char spk[96];
                if (spkOn) {
                    snprintf(spk, sizeof(spk), "; remote speaker %u underruns, %u samples dropped", spkUnder, spkDropped);
                } else {
                    snprintf(spk, sizeof(spk), "; remote speaker off");
                }
                port_log("audio: %u blocks, %u replayed (game late), %d device underruns, queued %.1f-%.1f ms; peak %d; mixer %u sub-frames, %.2f ms avg %.2f ms max; clock thread gap max %.1f ms, %u over 10 ms, %u over 20 ms; late blocks are played as silence, longest run %u blocks; %u DSP late (held)%s%s%s",
                         blocks, late, underruns - lastUnderruns, minQueued / 1e6, maxQueued / 1e6, peak, mixes, mixes ? mixNs / 1e6 / mixes : 0.0, mixMax / 1e6,
                         gapMax / 1e6, gapsOver, gapsOverLong, lateRunMax, dspHolds, spk, mt, hold);
            }
            lastUnderruns = underruns;
            gapMax = 0;
            gapsOver = gapsOverLong = 0;
            late = blocks = lateRunMax = 0;
            minQueued = INT64_MAX;
            maxQueued = -1;
            reportAt = now + 10000000000ll;
        }
    }
}

extern "C" {

void AIInit(u8* stack) {
    (void)stack;
    port_irq_set_handler(PORT_IRQ_AI, aiIrq);
    if (!sAiThreadStarted) {
        sAiThreadStarted = true;
        PortHostAllocScope scope;
        std::thread(aiClock).detach();
    }
}

AIDCallback AIRegisterDMACallback(AIDCallback cb) {
    AIDCallback old = sDmaCallback;
    sDmaCallback = cb;
    return old;
}

void AIInitDMA(u32 addr, u32 length) {
    sDmaAddr.store(addr);
    sDmaLen.store(length);
    sDmaLatched++;
}

void AIStartDMA(void) { sDmaRunning.store(true); }
void AIStopDMA(void) { sDmaRunning.store(false); }

void AISetDSPSampleRate(u32 rate) { sSampleRate.store(rate ? 48000 : 32000); }
u32 AIGetDSPSampleRate(void) { return sSampleRate.load() == 48000 ? 1 : 0; }

}  // extern "C"
