#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PortVitaMemoryBlock {
    void* base;
    uint32_t size;
    int32_t uid;
} PortVitaMemoryBlock;

typedef struct PortVitaImageSegment {
    void* base;
    uint32_t size;
} PortVitaImageSegment;

// Cached USER_RW only; size must be a nonzero multiple of 4096. Returns 0 or
// a negative error. If cleanup fails, out retains only a UID to retry freeing.
int port_vita_memory_alloc(const char* name, uint32_t size, PortVitaMemoryBlock* out);

// Leaves the descriptor unchanged on failure; clears it to {0, 0, -1} on success.
int port_vita_memory_free(PortVitaMemoryBlock* block);

// Returns separate, nonempty, 32-byte-aligned module ranges without modifying it.
// Failure leaves segments untouched and count zero; capacity must fit all ranges.
int port_vita_memory_image(PortVitaImageSegment* segments, uint32_t capacity, uint32_t* count);

// All three output pointers are required; query failures zero all valid outputs.
int port_vita_memory_free_sizes(uint32_t* user, uint32_t* cdram, uint32_t* phycont);

// CPU-only mapping query: 1 if the whole range is mapped, otherwise 0.
// Does not register, remap, or physically translate an unknown pointer.
int port_vita_memory_valid_range(const void* base, uint32_t size);

// Queries the current thread's actual SDK stack span without allocating or
// writing it. Required outputs are zeroed; returns 0 or a negative error.
int port_vita_thread_stack(void** base, uint32_t* size);
// Pins the calling thread to one of the three processor cores an application has.
// The port's plan: 0 the game's threads (only one of them runs at a time), 1 the renderer's
// back end (vitaGL and the sceGxm display queue), 2 the renderer's front end ("player"),
// audio output and the other helpers.
int port_vita_pin_core(int core);
int port_vita_thread_class(int cls);  // 1 clocks, 2 disc reader, 3 game and renderer, 4 background (see memory.cpp)

#ifdef __cplusplus
}
#endif
