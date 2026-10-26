#pragma once

#include <stdint.h>
#include "port/diagnostics.h"

#if !defined(__vita__) && !defined(PORT_ARMHF_HARNESS) && PORT_RUNTIME_DIAGNOSTICS && PORT_LOGGING
#define PORT_PHASE_CPU_COMPILED 1
#else
#define PORT_PHASE_CPU_COMPILED 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum PortGamePhase {
    PORT_GAME_PHASE_NONE,
    PORT_GAME_PHASE_BEGIN_RENDER,
    PORT_GAME_PHASE_DRAW,
    PORT_GAME_PHASE_END_RENDER,
    PORT_GAME_PHASE_UPDATE_RESET,
    PORT_GAME_PHASE_UPDATE_SCENE,
    PORT_GAME_PHASE_UPDATE_OBJECTS,
    PORT_GAME_PHASE_UPDATE_HOME,
    PORT_GAME_PHASE_UPDATE_ERROR,
    PORT_GAME_PHASE_UPDATE_DIMMING,
    PORT_GAME_PHASE_UPDATE_NERVE,
    PORT_GAME_PHASE_AUDIO_START,
    PORT_GAME_PHASE_AUDIO_UPDATE_SCENE,
    PORT_GAME_PHASE_AUDIO_GATE,
    PORT_GAME_PHASE_AUDIO_WAIT,
    PORT_GAME_PHASE_AUDIO_NERVE,
    PORT_GAME_PHASE_LOGO_REQUEST,
    PORT_GAME_PHASE_LOGO_UPDATE_SCENE,
    PORT_GAME_PHASE_CALC_ANIM,
    PORT_GAME_PHASE_CAPTURE,
    PORT_GAME_PHASE_END_FRAME,
    PORT_GAME_PHASE_WAIT_RETRACE,
    PORT_GAME_PHASE_FRAME_COMPLETE,
    PORT_GAME_PHASE_AUDIO_MOVEMENT,
    PORT_GAME_PHASE_AUDIO_CALC,
    PORT_GAME_PHASE_AUDIO_MIX,
    PORT_GAME_PHASE_AUDIO_OBJECTS,
    PORT_GAME_PHASE_FRAME_BEGIN,
    PORT_GAME_PHASE_AUDIO_RESOURCES,
    PORT_GAME_PHASE_AUDIO_JAS_INIT,
    PORT_GAME_PHASE_JAS_TRACK_POOL,
    PORT_GAME_PHASE_JAS_DVD_THREAD,
    PORT_GAME_PHASE_JAS_AUDIO_THREAD,
    PORT_GAME_PHASE_JAS_STREAM_INIT,
    PORT_GAME_PHASE_AUDIO_JAI_INIT,
    PORT_GAME_PHASE_AUDIO_POOLS,
    PORT_GAME_PHASE_AUDIO_SECTION,
    PORT_GAME_PHASE_AUDIO_ARCHIVE,
    PORT_GAME_PHASE_AUDIO_CONSTRUCT,
    PORT_GAME_PHASE_AUDIO_SCENE_MANAGER,
    PORT_GAME_PHASE_AUDIO_FINISH,
    PORT_GAME_PHASE_AUDIO_SPEAKER,
    PORT_GAME_PHASE_AUDIO_NAMES,
    PORT_GAME_PHASE_AUDIO_READY,
    PORT_GAME_PHASE_DSP_BOOT,
    PORT_GAME_PHASE_DSP_BUFFER,
    PORT_GAME_PHASE_DSP_CHANNELS,
    PORT_GAME_PHASE_DSP_CHANNEL_POOL,
    PORT_GAME_PHASE_AUDIO_DMA_START,
    PORT_GAME_PHASE_AUDIO_MESSAGE_WAIT,
    PORT_GAME_PHASE_FRAME_ALARM_CREATE,
    PORT_GAME_PHASE_FRAME_ALARM_SET,
    PORT_GAME_PHASE_FRAME_DRAW_DONE,
    PORT_GAME_PHASE_FRAME_RESET_SYNC,
    PORT_GAME_PHASE_FRAME_ALARM_CANCEL,
    PORT_GAME_PHASE_FRAME_FLUSH,
    PORT_GAME_PHASE_FRAME_RETRACE_COUNT,
    PORT_GAME_PHASE_GX_FINISH_FLUSH,
    PORT_GAME_PHASE_GX_FINISH_RESTORE,
    PORT_GAME_PHASE_GX_FINISH_WAIT,
    PORT_GAME_PHASE_END_GX_FLUSH,
    PORT_GAME_PHASE_COMBINATION_POSITIVE,
    PORT_GAME_PHASE_COMBINATION_ZERO,
    PORT_GAME_PHASE_COMBINATION_NEGATIVE,
    PORT_GAME_PHASE_COUNT
};

enum PortGameGpuPhase {
    PORT_GAME_GPU_NONE,
    PORT_GAME_GPU_WORKER_WAIT,
    PORT_GAME_GPU_CONSUME,
    PORT_GAME_GPU_PEEK,
    PORT_GAME_GPU_PRESENT,
    PORT_GAME_GPU_BEGIN,
    PORT_GAME_GPU_BEGIN_RETURN,
    PORT_GAME_GPU_END,
    PORT_GAME_GPU_END_RETURN,
    PORT_GAME_GPU_FINISH,
    PORT_GAME_GPU_FINISH_RETURN,
    PORT_GAME_GPU_HEARTBEAT,
    PORT_GAME_GPU_HEARTBEAT_RETURN,
    PORT_GAME_GPU_QUEUE,
    PORT_GAME_GPU_QUEUE_RETURN,
    PORT_GAME_GPU_INITIALIZE,
    PORT_GAME_GPU_CLOSE,
    PORT_GAME_GPU_DRAIN,
    PORT_GAME_GPU_DRAIN_RETURN,
    PORT_GAME_GPU_COUNT
};

typedef struct PortGameProgress {
    uint32_t phase;
    uint32_t generation;
    int64_t idle_ns;
    uint32_t audio_phase;
    uint32_t audio_generation;
    uint32_t dsp_phase;
    uint32_t dsp_generation;
    uint32_t irq_phase;
    uint32_t irq_generation;
    uint32_t gpu_phase;
    uint32_t gpu_generation;
    uint32_t cpu_owner;
} PortGameProgress;

// Diagnostic only: each lane has a coherent atomic checkpoint, not a shared
// cross-lane instant. Audio initialization and DSP work cannot hide main progress.
// No guest pointer or stack read. Publishing is lock-free, not wait-free.
// The 26-bit generation wraps; it is not a frame count or timing measurement.
void port_game_progress_checkpoint(uint32_t phase);
// One host observer, monotonic nanoseconds; at most four samples, five seconds apart.
int port_game_progress_poll(int64_t now, PortGameProgress* sample);
const char* port_game_progress_name(uint32_t phase);
// IRQ phase is 1+the original IRQ number while a handler runs, zero on return.
// CPU owner is a published identifier only, never a pointer for the observer to read.
void port_game_progress_irq_checkpoint(uint32_t irq, uint32_t active);
void port_game_progress_gpu_checkpoint(uint32_t phase);
void port_game_progress_cpu_checkpoint(uint32_t owner);
const char* port_game_progress_gpu_name(uint32_t phase);

#if PORT_PHASE_CPU_COMPILED
// Native, same-thread work intervals only; independent of the global progress lanes.
void port_native_phase_cpu_checkpoint(uint32_t phase);
#endif

#if defined(__vita__) || defined(PORT_ARMHF_HARNESS)
#define PORT_GAME_PROGRESS(phase) port_game_progress_checkpoint(phase)
#define PORT_GAME_GPU_PROGRESS(phase) port_game_progress_gpu_checkpoint(phase)
#elif PORT_PHASE_CPU_COMPILED
#define PORT_GAME_PROGRESS(phase) port_native_phase_cpu_checkpoint(phase)
#define PORT_GAME_GPU_PROGRESS(phase) ((void)0)
#else
#define PORT_GAME_PROGRESS(phase) ((void)0)
#define PORT_GAME_GPU_PROGRESS(phase) ((void)0)
#endif

#ifdef __cplusplus
}
#endif
