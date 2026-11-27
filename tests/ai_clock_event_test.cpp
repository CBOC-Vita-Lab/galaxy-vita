// GXV_AI_EVENT (thr24): the AI clock thread waits for the game's AIInitDMA on a condition variable instead of polling every 500 us.
// Links platform/src/audio/ai_dsp.cpp with a simulated audio device (a queue that drains in real time) and a simulated game that answers each AI
// interrupt after 0.3-3.3 ms (one in ten after 8-12 ms). Run twice (GXV_AI_EVENT=0 and =1, the runner does): the same blocks must be played in the same order, none played as
// silence (a late block), the device must never run dry, and with the switch on the clock must poll at most a tenth as often. (Latch to submit is printed for information: the
// clock holds a latched block back until the device queue is under its target, so it is not a measure of the wake-up.)  Output: one line "blocks N sleeps500 N meanLatchToSubmitUs N maxLatchToSubmitUs N hash H late N".
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "port/port.h"

typedef void (*AIDCallback)(void);
extern "C" {
void AIInit(uint8_t*);
AIDCallback AIRegisterDMACallback(AIDCallback);
void AIInitDMA(uint32_t addr, uint32_t length);
void AIStartDMA(void);
void AISetDSPSampleRate(uint32_t);
}

static int64_t Now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static PortIrqHandler sHandlers[PORT_IRQ_COUNT];
static std::atomic<int64_t> sQueueEnd{0};        // when the device runs dry
static std::atomic<int64_t> sLatchAt{0};         // when the game last latched a block
static std::atomic<int> sSleeps500{0}, sBlocks{0}, sUnder{0}, sLate{0};
static std::atomic<uint64_t> sHash{1469598103934665603ull};
static std::atomic<int64_t> sLatchToSubmitSum{0}, sLatchToSubmitMax{0};
static std::atomic<int> sAnswerPending{0};

extern "C" {
void port_irq_set_handler(int irq, PortIrqHandler handler) { sHandlers[irq] = handler; }
void port_irq_raise(int irq) { if (sHandlers[irq]) sHandlers[irq](); }
void port_log(const char*, ...) {}
void port_log_summary(const char*, ...) {}
int64_t port_host_time_ns(void) { return Now(); }
int64_t port_thread_cpu_us(void) { return 0; }
void port_vita_audio_clock_boost(void) {}
void port_vita_audio_mixer_boost(void) {}
void port_audio_irq_urgent(int) {}
void port_host_sleep_ns(int64_t ns) {
    if (ns == 500000) sSleeps500++;
    timespec ts{ns / 1000000000, ns % 1000000000};
    nanosleep(&ts, nullptr);
}
int port_is_paused(void) { return 0; }
int port_handheld(void) { return 0; }
void port_host_alloc_scope_enter(void) {}
void port_host_alloc_scope_leave(void) {}
int64_t port_audio_queued_ns(void) { const int64_t q = sQueueEnd.load() - Now(); return q < 0 ? 0 : q; }
void port_audio_submit(const int16_t* block, uint32_t frames, uint32_t rate) {
    const int64_t now = Now();
    const int64_t dur = static_cast<int64_t>(frames) * 1000000000ll / rate;
    int64_t end = sQueueEnd.load();
    if (end < now) { if (sBlocks.load() > 4) sUnder++; end = now; }
    sQueueEnd.store(end + dur);
    const int64_t d = now - sLatchAt.load();
    if (d >= 0 && d < 1000000000ll) { sLatchToSubmitSum += d; if (d > sLatchToSubmitMax.load()) sLatchToSubmitMax.store(d); }
    sHash.store((sHash.load() ^ static_cast<uint64_t>(reinterpret_cast<uintptr_t>(block) & 0xFFFF)) * 1099511628211ull);
    if (block[0] != 0x7777 && sBlocks.load() > 30) sLate++;  // (the first blocks play into an empty device)
    sBlocks++;
}
struct OSContext;
OSContext* OSGetCurrentContext(void) { return nullptr; }
void __DSPHandler(int, void*) {}
void port_heap_guard_register(const char*, const void* (*)(void*), void*) {}
int port_audio_underruns(void) { return 0; }
int port_audio_take_peak(void) { return 0; }
int port_audio_take_speaker_stats(unsigned*, unsigned*) { return 0; }
}

static void OnAiIrq() { sAnswerPending.store(1); }
static int16_t (*sBlockMem)[512 * 2];  // (below 4 GiB: the DMA address is 32 bits)

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? atoi(argv[1]) : 3;
    sBlockMem = static_cast<int16_t(*)[512 * 2]>(mmap(nullptr, 4 * sizeof(int16_t[512 * 2]), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0));
    if (sBlockMem == MAP_FAILED) { fprintf(stderr, "mmap\n"); return 2; }
    for (int i = 0; i < 4; ++i) for (auto& v : sBlockMem[i]) v = 0x7777;
    // (the blocks' content is not 0x7777 once "silence" is played: sHush is zeros / ramps; the block memory here stays 0x7777 so a block played from it reads as such)
    AIRegisterDMACallback(OnAiIrq);
    AISetDSPSampleRate(1);  // 48 kHz
    AIInitDMA(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sBlockMem[0])), 256 * 4);
    AIInit(nullptr);
    AIStartDMA();
    const int64_t end = Now() + seconds * 1000000000ll;
    uint32_t rng = 12345, n = 0;
    while (Now() < end) {
        if (!sAnswerPending.exchange(0)) { timespec ts{0, 100000}; nanosleep(&ts, nullptr); continue; }
        rng = rng * 1664525u + 1013904223u;
        const int64_t delayUs = ((rng >> 24) % 10 == 0) ? 8000 + (rng >> 8) % 4000 : 300 + (rng >> 8) % 3000;  // (1 in 10: a late turn of 8-12 ms)
        timespec ts{0, delayUs * 1000};
        nanosleep(&ts, nullptr);
        sLatchAt.store(Now());
        AIInitDMA(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sBlockMem[++n & 3])), 256 * 4);
    }
    const int blocks = sBlocks.load();
    printf("blocks %d sleeps500 %d meanLatchToSubmitUs %lld maxLatchToSubmitUs %lld underruns %d late %d\n", blocks, sSleeps500.load(),
           blocks ? static_cast<long long>(sLatchToSubmitSum.load() / blocks / 1000) : 0ll, static_cast<long long>(sLatchToSubmitMax.load() / 1000), sUnder.load(), sLate.load());
    return 0;
}
