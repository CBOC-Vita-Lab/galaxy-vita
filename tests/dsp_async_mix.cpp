// GXV_AUDIO_THREAD: the DSP mixer on a thread of its own must give the same bytes as the mixer inside the release.
//
// Links platform/src/audio/ai_dsp.cpp (the DSP mailbox) with platform/src/audio/dsp_mixer.cpp (the software mixer) and drives them
// the way JASDriver / JASDSPChannel do: 0x8E (VARAM base), 0x81 (table set-up), then per DAC block a 0x82 sync frame and, for each
// of its sub-frames, the guest's voice updates, the release packets of the four voice groups (the last one renders the
// sub-frame), and the wait for the reply mail, delivered through the DSP interrupt handler.  The script is deterministic.  What is
// hashed: every mail the guest reads, in order; every sub-frame's PCM; all voice blocks and the FX ring after each sub-frame.
// The runner (tools/test_dsp_async.sh) runs it with GXV_AUDIO_THREAD=0 and =1 and compares the digests; the test itself checks
// that the thread rendered exactly the sub-frames it should (and none with the switch off), that the mail order is the same when
// the guest sends a command (or a second release) before reading a reply, and that every wait finishes.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

#include "JSystem/JAudio2/JASDspInterface.hpp"
#include "port/port.h"

// --- what ai_dsp.cpp and dsp_mixer.cpp expect from the rest of the port -----------------------------------------------------
static PortIrqHandler sHandlers[PORT_IRQ_COUNT];
static std::mutex sIrqMutex;
static std::condition_variable sIrqCv;
static std::atomic<unsigned> sIrqPending{0};
extern "C" {
void port_irq_set_handler(int irq, PortIrqHandler handler) { sHandlers[irq] = handler; }
void port_irq_raise(int irq) {
    sIrqPending.fetch_or(1u << irq);
    std::lock_guard<std::mutex> lk(sIrqMutex);
    sIrqCv.notify_all();
}
void port_log(const char*, ...) {}
void port_log_summary(const char*, ...) {}
int64_t port_host_time_ns(void) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void port_host_sleep_ns(int64_t) {}
int port_is_paused(void) { return 0; }
int port_handheld(void) { return 0; }
void port_host_alloc_scope_enter(void) {}
void port_host_alloc_scope_leave(void) {}
OSContext* OSGetCurrentContext(void) { return nullptr; }

void DSPInit(void);
void DSPSendMailToDSP(uint32_t mail);
uint32_t DSPCheckMailFromDSP(void);
uint32_t DSPReadMailFromDSP(void);
unsigned port_dsp_mixer_thread_jobs(void);
}

// The interrupt handler the SDK would run: one event is a DCD1 mail and its payload.
static std::vector<uint32_t> sMails;
extern "C" void __DSPHandler(int, void*) {
    if (DSPCheckMailFromDSP() == 0) return;
    sMails.push_back(DSPReadMailFromDSP());
    sMails.push_back(DSPReadMailFromDSP());
}

static void Require(bool ok, const char* why) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", why);
        abort();
    }
}

// --- guest memory below 4 GiB (the mixer takes 32-bit addresses) -----------------------------------------------------------
static uint8_t* sArena;
static size_t sArenaUsed;
static constexpr size_t kArena = 8u << 20;
static void* Alloc(size_t bytes) {
    sArenaUsed = (sArenaUsed + 63) & ~size_t(63);
    void* p = sArena + sArenaUsed;
    sArenaUsed += bytes;
    Require(sArenaUsed <= kArena, "arena");
    return p;
}
static uint32_t Low(const void* p) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    Require(v < 0x100000000ull, "guest address fits 32 bits");
    return static_cast<uint32_t>(v);
}

static uint32_t sRng = 0x2545f491u;
static uint32_t Random() {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

// --- digest ----------------------------------------------------------------------------------------------------------------
static uint64_t sDigest = 1469598103934665603ull;
static void Hash(const void* data, size_t bytes) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; i++) sDigest = (sDigest ^ p[i]) * 1099511628211ull;
}
static std::vector<uint32_t> sAllMails;

// --- the guest's side of the mailbox ---------------------------------------------------------------------------------------
static void Send(const uint32_t* words, uint32_t count) {  // DSPSendCommands2: the count, then the words
    DSPSendMailToDSP(count);
    for (uint32_t i = 0; i < (count ? count : 1); i++) DSPSendMailToDSP(words[i]);
}
static void Release(unsigned group) {
    const uint32_t w0 = (group << 16) | 0x1234u;  // (DSP_CreateMap2's map is not looked at)
    Send(&w0, 0);
}
static void DeliverIrqs() {
    const unsigned bits = sIrqPending.exchange(0);
    if ((bits & (1u << PORT_IRQ_DSP)) && sHandlers[PORT_IRQ_DSP]) sHandlers[PORT_IRQ_DSP]();
}
// Waits for the interrupt like a guest that has nothing else to do, until `mixDone` sub-frame mails and `others` other replies have
// been read.  Returns false after 10 s.
static bool WaitMails(unsigned mixDone, unsigned others) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        DeliverIrqs();
        unsigned mix = 0, other = 0;
        for (size_t i = 0; i + 1 < sMails.size(); i += 2) (sMails[i + 1] == 0xF355FF00u ? mix : other)++;
        if (mix >= mixDone && other >= others) {
            for (uint32_t m : sMails) sAllMails.push_back(m);
            Hash(sMails.data(), sMails.size() * 4);
            sMails.clear();
            Require(mix == mixDone && other == others, "no more replies than were asked for");
            return true;
        }
        std::unique_lock<std::mutex> lk(sIrqMutex);
        if (sIrqCv.wait_until(lk, deadline, [] { return sIrqPending.load() != 0; }) == false) return false;
    }
}

// --- the voices and what the game does with them ---------------------------------------------------------------------------
static constexpr int kVoices = 0x40;
static constexpr int kAramBytes = 3 << 20;
using Voice = JASDsp::TChannel;
static Voice* sVoices;
static uint8_t* sAram;
static s16 *sOutL, *sOutR, *sFxRing;
static JASDsp::FxBuf* sFx;
static uint16_t* W(Voice* v) { return reinterpret_cast<uint16_t*>(v); }

static void InitVoice(int i) {
    Voice* v = &sVoices[i];
    memset(v, 0, sizeof *v);
    static const uint16_t kSources[] = {0, 1, 3, 4, 5, 7, 8, 9, 9, 9, 10, 11, 12, 16, 16};
    v->mIsActive = (i % 5) != 4;
    v->_8 = 1;  // reset: the DSP starts the voice from its set-up values
    v->mPitch = static_cast<uint16_t>(0x600 + Random() % 0x2400);
    if (i % 17 == 3) v->mPitch = static_cast<uint16_t>(0x4000 + Random() % 0x800);  // a rate of four or more: nearest-sample path
    v->_100 = kSources[Random() % (sizeof kSources / sizeof *kSources)];
    if (v->_100 == 5) v->_100 = 5;  // (2-bit AFC)
    v->_102 = (i % 3) != 0;                       // loops
    const uint32_t length = 3000 + Random() % 9000;
    v->_114 = length;                             // samples
    v->_110 = Random() % 512;                     // loop start
    v->_118 = 0x1000 + static_cast<uint32_t>(i) * 0xC000;  // start in ARAM
    v->_68 = 0;
    v->_104 = static_cast<s16>(Random());
    v->_106 = static_cast<s16>(Random());
    if (i % 4 == 1) v->mFilterMode = static_cast<uint16_t>(2 + Random() % 20);          // FIR
    if (i % 6 == 2) v->mFilterMode |= 0x20;                                              // biquad
    for (int k = 0; k < 8; k++) v->fir_filter_params[k] = static_cast<s16>(Random() & 0x3FFF);
    for (int k = 0; k < 4; k++) v->iir_filter_params[k] = static_cast<s16>(Random() & 0x3FFF);
    v->iir_filter_params[4] = (i % 7 == 0) ? static_cast<s16>(0x4000) : 0;               // low-pass
    if (i % 5 == 0) {  // positional mixing
        v->_58 = 1;
        v->_50 = static_cast<uint16_t>(((Random() & 0x7F) << 8) | (Random() & 0x7F));
        v->_52 = static_cast<uint16_t>(Random() & 0x3FFF);
        v->_54 = 0;
        v->_56 = static_cast<uint16_t>(0x2000 + Random() % 0x4000);
    } else {  // six destinations: front left and right, back, and two reverbs
        static const uint16_t kBuffers[6] = {0x0D00, 0x0D60, 0x0F40, 0x0CA0, 0x0E80, 0x0DC0};
        uint16_t* ch = W(v) + 0x08;
        for (int c = 0; c < 6; c++) {
            ch[c * 4] = kBuffers[c];
            ch[c * 4 + 1] = static_cast<uint16_t>(Random() % 0x3000);
            ch[c * 4 + 2] = static_cast<uint16_t>(Random() % 0x3000);
        }
    }
}

// What the audio thread does between two sub-frames: new volumes, pitch bends, pause, forced stop, a voice started again.
static void GuestUpdates(int block, int sub) {
    for (int i = 0; i < kVoices; i++) {
        Voice* v = &sVoices[i];
        v->_8 = 0;
        if (v->mIsFinished && (Random() & 15) == 0) {  // a new note on a freed voice
            const uint16_t keepSource = v->_100;
            InitVoice(i);
            v->_100 = keepSource;
        }
        if (v->_58) {
            v->_56 = static_cast<uint16_t>(0x1000 + Random() % 0x5000);
        } else {
            uint16_t* ch = W(v) + 0x08;
            for (int c = 0; c < 6; c++) ch[c * 4 + 1] = static_cast<uint16_t>(Random() % 0x3000);
        }
        if ((block * 7 + sub + i) % 11 == 0) v->mPitch = static_cast<uint16_t>(0x600 + Random() % 0x2400);
        v->mPauseFlag = ((block * 7 + sub + i) % 29 == 0) ? 1 : 0;
        v->mForcedStop = ((block * 7 + sub + i * 3) % 211 == 0) ? 1 : 0;
    }
}

static void HashState(unsigned sub, int block) {
    Hash(sOutL + sub * 80, 160);
    Hash(sOutR + sub * 80, 160);
    Hash(sVoices, sizeof(Voice) * kVoices);
    Hash(sFxRing, 4 * 8 * 160);
    (void)block;
}

int main(int, char** argv) {
    const char* e = getenv("GXV_AUDIO_THREAD");
    const bool expectThread = e && *e && strtol(e, nullptr, 0) == 1;
#ifdef MAP_32BIT
    sArena = static_cast<uint8_t*>(mmap(nullptr, kArena, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0));
#else
    sArena = static_cast<uint8_t*>(mmap(reinterpret_cast<void*>(0x20000000), kArena, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
#endif
    Require(sArena != MAP_FAILED && reinterpret_cast<uintptr_t>(sArena) + kArena < 0x100000000ull, "mapping below 4 GiB");

    sVoices = static_cast<Voice*>(Alloc(sizeof(Voice) * kVoices));
    sAram = static_cast<uint8_t*>(Alloc(kAramBytes));
    uint32_t* table = static_cast<uint32_t*>(Alloc(0x140 * 4));
    uint8_t* afc = static_cast<uint8_t*>(Alloc(0x40));
    sFx = static_cast<JASDsp::FxBuf*>(Alloc(sizeof(JASDsp::FxBuf) * 4));
    sFxRing = static_cast<s16*>(Alloc(4 * 8 * 160));
    constexpr unsigned kBlocks = 36, kSubs = 7;
    sOutL = static_cast<s16*>(Alloc(kSubs * 80 * 2));
    sOutR = static_cast<s16*>(Alloc(kSubs * 80 * 2));
    for (int i = 0; i < kAramBytes; i++) sAram[i] = static_cast<uint8_t>(Random());
    for (int i = 0; i < 0x140; i++) table[i] = Random();
    for (int i = 0; i < 0x40; i++) afc[i] = static_cast<uint8_t>(Random() & 0x3F);  // (coefficients of moderate size)
    for (int i = 0; i < 4; i++) {
        JASDsp::FxBuf& f = sFx[i];
        memset(&f, 0, sizeof f);
        f._0 = (i % 2) ? 3 : 1;
        f._2 = 4;
        f._4 = sFxRing + i * 8 * 20;
        f._8 = 0x0D00;
        f._A = 0x2000;
        f._C = 0x0D60;
        f._E = 0x1800;
        for (int k = 0; k < 8; k++) f._10[k] = static_cast<uint16_t>(Random() & 0x3FFF);
    }
    for (int i = 0; i < kVoices; i++) InitVoice(i);

    DSPInit();  // registers the interrupt handler

    // 0x8E: the VARAM base; 0x81: the tables.  Each is answered synchronously.
    { const uint32_t w[2] = {0x8E000000u, Low(sAram)}; Send(w, 2); }
    Require(WaitMails(0, 1), "VARAM reply");
    { const uint32_t w[5] = {0x81000000u | kVoices, Low(sVoices), Low(table), Low(afc), Low(sFx)}; Send(w, 5); }
    Require(WaitMails(0, 1), "table set-up reply");

    unsigned rendered = 0;
    for (unsigned block = 0; block < kBlocks; block++) {
        const uint32_t sync[5] = {0x82000000u | (kSubs << 16) | (0x3000u + block * 0x40), Low(sOutL), Low(sOutR), 0, 0};
        Send(sync, 5);
        for (unsigned sub = 0; sub < kSubs; sub++) {
            GuestUpdates(static_cast<int>(block), static_cast<int>(sub));
            Release(0);
            Release(1);
            Release(2);
            Release(3);  // renders the sub-frame
            rendered++;
            if (block % 4 == 2 && sub == 3) {
                // A command straight behind the release, before its reply is read: the DSP answers in order, the sub-frame first.
                const uint32_t w[2] = {0x8E000000u, Low(sAram)};
                Send(w, 2);
                Require(WaitMails(1, 1), "release then VARAM: both replies");
            } else if (block % 5 == 3 && sub == 2 && sub + 1 < kSubs) {
                // Two releases in a row (the game breaking the lock-step): both are rendered, in order, two replies.  (The voice
                // blocks are not touched in between: on the console the DSP would be reading them.)
                Release(0);
                Release(1);
                Release(2);
                Release(3);
                rendered++;
                Require(WaitMails(2, 0), "two releases: two replies");
                HashState(sub, static_cast<int>(block));
                sub++;
            } else {
                Require(WaitMails(1, 0), "reply to the release");
            }
            HashState(sub, static_cast<int>(block));
        }
        // The next block's sync frame is only sent once the last sub-frame has been answered (as JASDriver does).
        if (block % 9 == 4) {
            // a table set-up in the middle of the stream: the mixer's state is replaced between sub-frames
            const uint32_t w[5] = {0x81000000u | kVoices, Low(sVoices), Low(table), Low(afc), Low(sFx)};
            Send(w, 5);
            Require(WaitMails(0, 1), "table set-up reply (middle)");
        }
    }

    const unsigned jobs = port_dsp_mixer_thread_jobs();
    if (expectThread) {
        Require(jobs == rendered, "every sub-frame was rendered by the mixer thread");
    } else {
        Require(jobs == 0, "no sub-frame was rendered on the mixer thread with the switch off");
    }
    // Something must have been mixed: all-zero PCM would make the comparison meaningless.
    bool nonZero = false;
    for (unsigned i = 0; i < kSubs * 80; i++) nonZero |= sOutL[i] != 0 || sOutR[i] != 0;
    Require(nonZero, "the script produced audible PCM");
    Require(sAllMails.size() >= rendered * 2, "a reply per sub-frame");

    printf("digest %016llx mails %zu sub-frames %u mixer-thread %u\n", static_cast<unsigned long long>(sDigest), sAllMails.size() / 2, rendered, jobs);
    if (argv[1]) {
        FILE* f = fopen(argv[1], "w");
        Require(f != nullptr, "digest file");
        fprintf(f, "%016llx %zu %u\n", static_cast<unsigned long long>(sDigest), sAllMails.size() / 2, rendered);
        fclose(f);
    }
    return 0;
}
