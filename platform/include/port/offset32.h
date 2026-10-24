#pragma once

#include "port/port.h"
#include <stdint.h>

// Cooked byte offsets may encode a backward relocation modulo 2^32.
template <typename T>
inline T* port_offset32(T* base, uint32_t offset) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(base);
    if (address > UINT32_MAX) port_fatal("32-bit resource offset has an invalid native base %p", base);
    const uint32_t resolved = static_cast<uint32_t>(address) + offset;
    return reinterpret_cast<T*>(static_cast<uintptr_t>(resolved));
}
