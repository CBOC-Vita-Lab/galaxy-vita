// Hooks for the armhf instruction-counting harness (armhf/, scripts/arm-*.sh): marks and thread names the qemu-user
// plugin (scripts/arm-plugin/smgprof.c) picks up.  Compiled to nothing outside PORT_ARMHF_HARNESS.
#pragma once
#include <stdint.h>

#if defined(PORT_ARMHF_HARNESS)
#ifdef __cplusplus
extern "C" {
#endif
// A game frame was finished (vgx display copy): argument = frames completed so far.
void port_qemu_mark(uint32_t frame);
// Names the calling host thread for the per-thread report (up to 15 characters).
void port_qemu_thread(const char* name);
// Names a Wii OSThread's host thread by its entry function and parameter (the report resolves the symbol / vtable).
void port_qemu_osthread(void* entry, void* param);
// The finished frame's workload counters (GX primitives, decoded vertices, triangles, draw records, command streams): for
// recognising the scene in the log.  One syscall, a few instructions.
void port_qemu_frame_stats(uint32_t frame, uint32_t prims, uint32_t verts, uint32_t tris, uint32_t draws, uint32_t streams);
#ifdef __cplusplus
}
#endif
#define PORT_QEMU_MARK(frame) port_qemu_mark(frame)
#define PORT_QEMU_FRAME_STATS(f, p, v, t, d, s) port_qemu_frame_stats(f, p, v, t, d, s)
#define PORT_QEMU_THREAD(name) port_qemu_thread(name)
#define PORT_QEMU_OSTHREAD(entry, param) port_qemu_osthread((void*)(entry), (void*)(param))
#else
#define PORT_QEMU_MARK(frame) ((void)(frame))
#define PORT_QEMU_FRAME_STATS(f, p, v, t, d, s) ((void)0)
#define PORT_QEMU_THREAD(name) ((void)0)
#define PORT_QEMU_OSTHREAD(entry, param) ((void)0)
#endif
