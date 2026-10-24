// Marks and thread names for the qemu-user plugin (scripts/arm-plugin/smgprof.c).  They are syscalls with numbers
// the kernel does not know (the plugin's syscall callback sees them; qemu answers ENOSYS).  Only issued with
// PETARI_QEMU_MARK=1, so the same binary runs normally on real hardware.
#include "port/qemu_hooks.h"

#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "port/port.h"
#include "port/vclock.h"

namespace {
enum { kMark = 0x7f00, kThread = 0x7f01, kStats = 0x7f02 };
bool enabled() {
    static const bool on = getenv("PETARI_QEMU_MARK") && atoi(getenv("PETARI_QEMU_MARK")) != 0;
    return on;
}
}  // namespace

extern "C" void port_qemu_mark(uint32_t frame) {
    if (enabled()) syscall(kMark, frame);
}

extern "C" void port_qemu_thread(const char* name) {
    if (port_vclock_enabled()) port_vclock_name_thread(name);
    if (!enabled()) return;
    uint32_t w[4] = {};
    strncpy(reinterpret_cast<char*>(w), name, 15);
    syscall(kThread, 1u, w[0], w[1], w[2], w[3]);
}

extern "C" void port_qemu_osthread(void* entry, void* param) {
    if (!enabled()) return;
    uint32_t vptr = 0;
    if (param && port_is_native_range(param, 4)) vptr = *static_cast<uint32_t*>(param);  // JKRThread & co: the vtable
    syscall(kThread, 2u, (uint32_t)(uintptr_t)entry, (uint32_t)(uintptr_t)param, vptr, 0u);
}

extern "C" void port_qemu_frame_stats(uint32_t frame, uint32_t prims, uint32_t verts, uint32_t tris, uint32_t draws, uint32_t streams) {
    if (enabled()) syscall(kStats, frame, prims, verts, tris, draws, streams);
}
