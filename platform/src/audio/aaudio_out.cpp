// Host audio output: the game's AI DMA stream (16-bit stereo, right sample
// first, normally 32 kHz) played through AAudio.  Blocks go into a ring that
// AAudio's data callback drains at the device's pace; the AI clock
// (ai_dsp.cpp) keeps the ring topped up.  PETARI_WAV=<path> also records the
// stream to a WAV file.
#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#elif defined(__vita__)
#include "audio.h"
#elif defined(PORT_ARMHF_HARNESS)
// armhf/qemu harness: the Vita's output path (48 kHz device, resampler, ring), thread in armhf/glue/audio_out.cpp
#define PORT_VITA_LIKE_AUDIO 1
extern "C" {
typedef void (*PortVitaAudioDrain)(void* stereoS16, int32_t frames);
int port_vita_audio_open(PortVitaAudioDrain drain);
void port_vita_audio_close(void);
int port_vita_audio_failed(void);
}
#else
#include <SDL2/SDL.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "port/port.h"
#include "revolution/types.h"

namespace {

std::mutex sLock;  // the stream, the resampler and the WAV file
#ifdef __ANDROID__
AAudioStream* sStream = nullptr;
#elif defined(__vita__) || defined(PORT_VITA_LIKE_AUDIO)
bool sStream = false;
#else
SDL_AudioDeviceID sStream = 0;
#endif
bool sOpenFailed = false;
std::atomic<bool> sDisconnected{false};
int32_t sDeviceRate = 0;
std::vector<s16> sLR, sOut;

// Stereo resampling with 4-point cubic (Catmull-Rom) interpolation, across
// block boundaries.
struct Resampler {
    s16 hist[3][2] = {};  // the previous block's last 3 frames
    double pos = 1.0;     // next output position; index 0 = hist[0]

    void process(const s16* in, u32 frames, double step, std::vector<s16>& out) {
        auto at = [&](int k, int c) -> float { return k < 3 ? hist[k][c] : in[(k - 3) * 2 + c]; };
        int total = (int)frames + 3;
        for (;;) {
            int i = (int)pos;
            if (i + 2 >= total) break;
            float t = (float)(pos - i);
            for (int c = 0; c < 2; c++) {
                float p0 = at(i - 1, c), p1 = at(i, c), p2 = at(i + 1, c), p3 = at(i + 2, c);
                float v = p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
                out.push_back((s16)(v < -32768.0f ? -32768.0f : v > 32767.0f ? 32767.0f : v));
            }
            pos += step;
        }
        s16 last[3][2];
        for (int k = 0; k < 3; k++) {
            last[k][0] = (s16)at(total - 3 + k, 0);
            last[k][1] = (s16)at(total - 3 + k, 1);
        }
        memcpy(hist, last, sizeof(hist));
        pos -= frames;
    }
};
Resampler sResampler;

// Interleaved L/R frames at the device rate: written by port_audio_submit,
// read by the data callback.  Positions count frames and wrap freely.
const u32 kRingFrames = 16384;  // 0.5 s at 32 kHz
s16 sRing[kRingFrames * 2];
std::atomic<u32> sRingWrite{0}, sRingRead{0};
std::atomic<int> sUnderruns{0};
bool sPlaying = false;  // callback thread only: the ring had audio last time

FILE* sWav = nullptr;
u32 sWavFrames = 0;
u32 sWavRate = 0;
bool sWavFinished = false;

// Output gain: silent while a cutscene is fast-forwarded (its sounds would
// come out in a jumble), with 30 ms ramps.
float sGain = 1.0f;

void applySkipGain(s16* lr, u32 frames, u32 rate) {
    float target = port_skip_fast_forwarding() ? 0.0f : 1.0f;
    if (sGain == target && target == 1.0f) {
        return;
    }
    float step = 1.0f / (rate * 0.03f);
    for (u32 i = 0; i < frames; i++) {
        sGain = target > sGain ? fminf(target, sGain + step) : fmaxf(target, sGain - step);
        lr[i * 2] = (s16)(lr[i * 2] * sGain);
        lr[i * 2 + 1] = (s16)(lr[i * 2 + 1] * sGain);
    }
}

// Wii remote speaker PCM (6 kHz mono), mixed into the output while the emulated speaker is on (port_remote_speaker_enabled
// in wpad.cpp: off by default on a handheld, where the game then uses its TV-side fallback sounds instead).
//
// The game hands it over 40 samples (6.7 ms) at a time from an OSAlarm that, in this port, only runs at the guest's safe
// points, so it arrives in bursts.  Played straight off the ring with linear interpolation it cut in and out with a click
// at every burst gap and left the 6 kHz images (a gritty, aliased copy) on top of the music.  It now starts once 40 ms are
// queued, fades out instead of stopping dead when the ring runs dry (and fades back in), caps the backlog at 200 ms, and is
// interpolated with Catmull-Rom and low-passed at 2.8 kHz (4th-order Butterworth) at the output rate.
extern "C" int port_remote_speaker_enabled(void);
const u32 kSpeakerRate = 6000;
const u32 kSpkRing = 4096;          // power of two
const u32 kSpkPrime = 240;          // 40 ms queued before (re)starting
const u32 kSpkMaxQueued = 1200;     // 200 ms: an older backlog is dropped (it would only lag behind the picture)
const float kSpkGain = 0.55f;       // the speaker sat in the player's hand, under the TV mix
std::mutex sSpkLock;
s16 sSpk[kSpkRing];
u32 sSpkRead = 0, sSpkWrite = 0;  // ring positions (free-running)
u32 sSpkLastWrite = 0;            // sSpkWrite at the previous block
double sSpkPhase = 0.0;
bool sSpkPlaying = false;
float sSpkPrev = 0.0f;   // the sample before sSpkRead (Catmull-Rom p0)
float sSpkHold = 0.0f;   // last interpolated value; dies away after the ring ran dry
float sSpkFade = 0.0f;   // fade-in after a (re)start
u32 sSpkQuiet = 0;       // output samples since the filters last saw a non-zero input
std::atomic<u32> sSpkUnderruns{0}, sSpkDropped{0};

struct SpkBiquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    float run(float x) {  // transposed direct form II
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    void lowPass(float fc, float q, float fs) {  // RBJ cookbook
        const float w = 2.0f * 3.14159265f * fc / fs, c = cosf(w), alpha = sinf(w) / (2.0f * q), a0 = 1.0f + alpha;
        b0 = (1.0f - c) * 0.5f / a0;
        b1 = (1.0f - c) / a0;
        b2 = b0;
        a1 = -2.0f * c / a0;
        a2 = (1.0f - alpha) / a0;
        z1 = z2 = 0.0f;
    }
};
SpkBiquad sSpkLp[2];
u32 sSpkLpRate = 0;

void mixSpeaker(s16* lr, u32 frames, u32 rate) {
    std::lock_guard<std::mutex> lock(sSpkLock);
    const u32 mask = kSpkRing - 1;
    u32 queued = sSpkWrite - sSpkRead;
    // A sound shorter than the jitter buffer still plays: once nothing new came in since the last block, start anyway.
    const bool producerIdle = sSpkWrite == sSpkLastWrite;
    sSpkLastWrite = sSpkWrite;
    if (!sSpkPlaying && queued < kSpkPrime && !(producerIdle && queued >= 3) && sSpkQuiet > 1024) {
        return;  // nothing playing and the filters have rung out
    }
    if (sSpkLpRate != rate && rate) {
        sSpkLp[0].lowPass(2800.0f, 0.5412f, (float)rate);
        sSpkLp[1].lowPass(2800.0f, 1.3066f, (float)rate);
        sSpkLpRate = rate;
    }
    if (queued > kSpkMaxQueued) {
        const u32 skip = queued - kSpkPrime;
        sSpkRead += skip;
        sSpkPrev = sSpk[(sSpkRead - 1) & mask];
        sSpkDropped.fetch_add(skip, std::memory_order_relaxed);
        queued = kSpkPrime;
    }
    if (!sSpkPlaying && (queued >= kSpkPrime || (producerIdle && queued >= 3))) {
        sSpkPlaying = true;
        sSpkFade = 0.0f;
    }
    const double step = (double)kSpeakerRate / (double)(rate ? rate : 32000);
    for (u32 i = 0; i < frames; i++) {
        if (sSpkPlaying && sSpkWrite - sSpkRead < 3) {
            sSpkPlaying = false;
            sSpkUnderruns.fetch_add(1, std::memory_order_relaxed);
        }
        float x;
        if (sSpkPlaying) {
            const float p0 = sSpkPrev, p1 = sSpk[sSpkRead & mask], p2 = sSpk[(sSpkRead + 1) & mask], p3 = sSpk[(sSpkRead + 2) & mask];
            const float t = (float)sSpkPhase;
            x = p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
            if (sSpkFade < 1.0f) {
                sSpkFade += 1.0f / 64.0f;
                x *= sSpkFade < 1.0f ? sSpkFade : 1.0f;
            }
            sSpkHold = x;
            sSpkPhase += step;
            while (sSpkPhase >= 1.0) {
                sSpkPhase -= 1.0;
                sSpkPrev = sSpk[sSpkRead & mask];
                sSpkRead++;
            }
        } else {
            sSpkHold *= 0.96f;  // about 2 ms to die away
            if (sSpkHold > -1.0f && sSpkHold < 1.0f) sSpkHold = 0.0f;
            x = sSpkHold;
        }
        sSpkQuiet = x != 0.0f ? 0 : sSpkQuiet + 1;
        const float y = sSpkLp[1].run(sSpkLp[0].run(x)) * kSpkGain;
        const int s = (int)y;
        for (int c = 0; c < 2; c++) {
            const int v = lr[i * 2 + c] + s;
            lr[i * 2 + c] = (s16)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
        }
    }
    if (!sSpkPlaying && sSpkQuiet > 1024) {
        sSpkLp[0].z1 = sSpkLp[0].z2 = sSpkLp[1].z1 = sSpkLp[1].z2 = 0.0f;  // (no denormal tail)
    }
}

void writeWavHeader(FILE* f, u32 frames, u32 rate) {
    u32 dataBytes = frames * 4;
    u8 h[44];
    memcpy(h, "RIFF", 4);
    u32 v = 36 + dataBytes;
    memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;
    memcpy(h + 16, &v, 4);
    u16 s = 1;
    memcpy(h + 20, &s, 2);  // PCM
    s = 2;
    memcpy(h + 22, &s, 2);  // stereo
    memcpy(h + 24, &rate, 4);
    v = rate * 4;
    memcpy(h + 28, &v, 4);
    s = 4;
    memcpy(h + 32, &s, 2);
    s = 16;
    memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &dataBytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}

// AAudio's real-time thread: copies queued frames out, silence when there
// are none.
void drainAudio(void* audioData, int32_t numFrames) {
    s16* out = (s16*)audioData;
    u32 r = sRingRead.load(std::memory_order_relaxed);
    u32 avail = sRingWrite.load(std::memory_order_acquire) - r;
    u32 n = avail < (u32)numFrames ? avail : (u32)numFrames;
    for (u32 i = 0; i < n; i++) {
        u32 idx = (r + i) & (kRingFrames - 1);
        out[i * 2] = sRing[idx * 2];
        out[i * 2 + 1] = sRing[idx * 2 + 1];
    }
    if (n < (u32)numFrames) {
        memset(out + n * 2, 0, (size_t)(numFrames - n) * 4);
        if (sPlaying && !port_is_paused()) {
            sUnderruns++;
        }
        sPlaying = false;
    } else {
        sPlaying = true;
    }
    sRingRead.store(r + n, std::memory_order_release);
}

#ifdef __ANDROID__
aaudio_data_callback_result_t dataCallback(AAudioStream*, void*, void* audioData, int32_t numFrames) {
    drainAudio(audioData, numFrames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

// The output device went away or changed: port_audio_submit reopens the
// stream (the callback may not do it itself).
void errorCallback(AAudioStream*, void*, aaudio_result_t error) {
    if (error == AAUDIO_ERROR_DISCONNECTED) {
        sDisconnected = true;
    }
}
#elif !defined(__vita__) && !defined(PORT_VITA_LIKE_AUDIO)
void dataCallback(void*, Uint8* audioData, int bytes) { drainAudio(audioData, bytes / 4); }
#endif

bool openStream(u32 rate) {
#ifdef __ANDROID__
    AAudioStreamBuilder* b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return false;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    // The device's own rate (48 kHz on the Quest): asking for the game's
    // 32 kHz moves AAudio off its fast path, to 20 ms bursts.
    (void)rate;
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setDataCallback(b, dataCallback, nullptr);
    AAudioStreamBuilder_setErrorCallback(b, errorCallback, nullptr);
    aaudio_result_t r = AAudioStreamBuilder_openStream(b, &sStream);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        port_log("audio: AAudio open failed: %s", AAudio_convertResultToText(r));
        sStream = nullptr;
        return false;
    }
    sDeviceRate = AAudioStream_getSampleRate(sStream);
    // The device-side queue: 3 bursts (12 ms on the Quest) on top of the
    // ring, instead of the default 8.
    int32_t burst = AAudioStream_getFramesPerBurst(sStream);
    if (burst > 0) {
        AAudioStream_setBufferSizeInFrames(sStream, burst * 3);
    }
    sRingRead.store(sRingWrite.load());
    sResampler = Resampler();
    AAudioStream_requestStart(sStream);
    port_log("audio: AAudio %d Hz (source %u Hz), burst %d frames, buffer %d frames", sDeviceRate, rate, burst, AAudioStream_getBufferSizeInFrames(sStream));
#elif defined(__vita__) || defined(PORT_VITA_LIKE_AUDIO)
    sDeviceRate = 48000;
    sRingRead.store(sRingWrite.load());
    sResampler = Resampler();
    sStream = port_vita_audio_open(drainAudio) != 0;
    if (!sStream) {
        port_log("audio: Vita output open failed");
        return false;
    }
    port_log("audio: Vita 48000 Hz (source %u Hz), burst 512 frames", rate);
#else
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        port_log("audio: SDL init failed: %s", SDL_GetError());
        return false;
    }
    SDL_AudioSpec wanted{}, actual{};
    wanted.freq = 48000;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = 512;
    wanted.callback = dataCallback;
    sStream = SDL_OpenAudioDevice(nullptr, 0, &wanted, &actual, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!sStream) {
        port_log("audio: SDL open failed: %s", SDL_GetError());
        return false;
    }
    sDeviceRate = actual.freq;
    sRingRead.store(sRingWrite.load());
    sResampler = Resampler();
    SDL_PauseAudioDevice(sStream, 0);
    port_log("audio: SDL driver=%s %d Hz (source %u Hz), burst %u frames",
             SDL_GetCurrentAudioDriver(), sDeviceRate, rate, actual.samples);
#endif
    return true;
}

}  // namespace

extern "C" void port_audio_submit(const s16* rightLeft, u32 frames, u32 rate) {
    std::lock_guard<std::mutex> lock(sLock);
    sLR.resize(frames * 2);
    for (u32 i = 0; i < frames; i++) {
        sLR[i * 2] = rightLeft[i * 2 + 1];
        sLR[i * 2 + 1] = rightLeft[i * 2];
    }
    mixSpeaker(sLR.data(), frames, rate);
    applySkipGain(sLR.data(), frames, rate);

    static bool wavChecked = false;
    if (!wavChecked && !sWavFinished) {
        wavChecked = true;
        if (const char* path = getenv("PETARI_WAV")) {
            if (*path && (sWav = fopen(path, "wb")) != nullptr) {
                writeWavHeader(sWav, 0, rate);
            }
        }
    }
    if (sWav) {
        sWavRate = rate;
        fwrite(sLR.data(), 4, frames, sWav);
        sWavFrames += frames;
        if ((sWavFrames & 0x3FFF) < frames) {
            writeWavHeader(sWav, sWavFrames, rate);
            fflush(sWav);
        }
    }

#if defined(__vita__) || defined(PORT_VITA_LIKE_AUDIO)
    if (sStream && port_vita_audio_failed()) sDisconnected.store(true);
#endif
    if (sStream && sDisconnected.exchange(false)) {
        port_log("audio: output device disconnected, reopening");
#ifdef __ANDROID__
        AAudioStream_close(sStream);
        sStream = nullptr;
#elif defined(__vita__) || defined(PORT_VITA_LIKE_AUDIO)
        port_vita_audio_close();
        sStream = false;
#else
        SDL_CloseAudioDevice(sStream);
        sStream = 0;
#endif
    }
    if (!sStream && !sOpenFailed) {
        sOpenFailed = !openStream(rate);
    }
    if (!sStream) return;

    const s16* out = sLR.data();
    u32 outFrames = frames;
    if ((u32)sDeviceRate != rate) {
        sOut.clear();
        sResampler.process(sLR.data(), frames, (double)rate / (double)sDeviceRate, sOut);
        out = sOut.data();
        outFrames = (u32)(sOut.size() / 2);
    }

    u32 w = sRingWrite.load(std::memory_order_relaxed);
    u32 space = kRingFrames - (w - sRingRead.load(std::memory_order_acquire));
    if (outFrames > space) {
        outFrames = space;  // the device stopped reading; the AI clock waits for it
    }
    for (u32 i = 0; i < outFrames; i++) {
        u32 idx = (w + i) & (kRingFrames - 1);
        sRing[idx * 2] = out[i * 2];
        sRing[idx * 2 + 1] = out[i * 2 + 1];
    }
    sRingWrite.store(w + outFrames, std::memory_order_release);
}

extern "C" void port_speaker_push(const s16* samples, int count) {
    if (!port_remote_speaker_enabled()) return;
    std::lock_guard<std::mutex> lock(sSpkLock);
    for (int i = 0; i < count; i++) {
        if (sSpkWrite - sSpkRead >= kSpkRing) {  // drop the oldest when the output stalls
            sSpkRead++;
            sSpkDropped.fetch_add(1, std::memory_order_relaxed);
        }
        sSpk[sSpkWrite & (kSpkRing - 1)] = samples[i];
        sSpkWrite++;
    }
}

// Remote speaker stream health since the last call (for the audio: log line): times its jitter buffer ran dry, and
// samples dropped as backlog.  Returns 0 while the emulated speaker is off.
extern "C" int port_audio_take_speaker_stats(unsigned* underruns, unsigned* dropped) {
    if (!port_remote_speaker_enabled()) return 0;
    if (underruns) *underruns = sSpkUnderruns.exchange(0, std::memory_order_relaxed);
    if (dropped) *dropped = sSpkDropped.exchange(0, std::memory_order_relaxed);
    return 1;
}

extern "C" void port_audio_capture_finish(void) {
    std::lock_guard<std::mutex> lock(sLock);
    sWavFinished = true;
    if (!sWav) return;
    writeWavHeader(sWav, sWavFrames, sWavRate);
    fclose(sWav);
    sWav = nullptr;
}

#if defined(__vita__)
// The loudest sample sent to the console's audio port since the last call.
extern "C" int port_audio_take_peak(void) { return port_vita_audio_take_peak(nullptr, nullptr); }
#endif

// Times the device found the ring empty while the game was running.
extern "C" int port_audio_underruns(void) { return sUnderruns.load(); }

// Audio queued for the device, or -1 without one (the AI then runs on a
// timer).
extern "C" int64_t port_audio_queued_ns(void) {
    std::lock_guard<std::mutex> lock(sLock);
#if defined(__vita__) || defined(PORT_VITA_LIKE_AUDIO)
    if (sStream && port_vita_audio_failed()) {
        sDisconnected.store(true);
        return -1;
    }
#endif
    if (!sStream || sDeviceRate <= 0) return sOpenFailed ? -1 : 0;
    u32 queued = sRingWrite.load(std::memory_order_relaxed) - sRingRead.load(std::memory_order_acquire);
    return (int64_t)queued * 1000000000ll / sDeviceRate;
}
