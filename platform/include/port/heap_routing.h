// Decides whether a C++ allocation belongs in the game's JKRHeap tree or in the host heap.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// True when the calling thread is an emulated game thread (OSThread) that is not
// inside a PortHostAllocScope.
int port_use_game_heap(void);

// True when p lies inside the emulated MEM1/MEM2 arenas that back the JKR heaps.
int port_is_game_heap_ptr(const void* p);

void* port_host_alloc(size_t size, int align);
void port_host_free(void* p);

// Called when a C++ `new` could not get host memory: reports the heap and aborts. (A
// non-nothrow operator new must never return null: std::vector and every other container
// use the result unchecked. The Vita's 56 MiB C heap ran out during the opening
// cutscene's scene change, a vector's storage came back null with a capacity of a few MiB,
// and the vertex decoder stored through address 0x002F1080.)
void port_host_alloc_failed(size_t size, int align) __attribute__((noreturn));

// One log line with the C heap's state (Vita; elsewhere nothing): arena, in use, free.
void port_host_heap_log(const char* when);

void port_host_alloc_scope_enter(void);
void port_host_alloc_scope_leave(void);

#ifdef __cplusplus
}

// RAII guard for platform code that runs on a game thread but must allocate from
// the host heap (std containers inside the renderer, file system, etc).
struct PortHostAllocScope {
    PortHostAllocScope() { port_host_alloc_scope_enter(); }
    ~PortHostAllocScope() { port_host_alloc_scope_leave(); }
    PortHostAllocScope(const PortHostAllocScope&) = delete;
    PortHostAllocScope& operator=(const PortHostAllocScope&) = delete;
};
#endif
