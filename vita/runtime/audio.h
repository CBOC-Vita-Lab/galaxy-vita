#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The existing mixer/ring/resampler supplies 512 interleaved L/R frames.
// Lifecycle calls belong to one control thread; drain runs on the audio thread.
typedef void (*PortVitaAudioDrain)(void* stereoS16, int32_t frames);
int port_vita_audio_open(PortVitaAudioDrain drain);
void port_vita_audio_close(void);
int port_vita_audio_failed(void);
int port_vita_audio_take_peak(unsigned* outputCalls, unsigned* errors);

#ifdef __cplusplus
}
#endif
