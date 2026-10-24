#pragma once

#include <stdint.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "gd_write.h byte-swaps for a little-endian host"
#endif

// The display-list payload is separate from its cursor metadata. Keep each
// complete word's cursor in a register rather than reloading it after each byte.
// The bytes are stored big-endian (the order the console's GPU reads them) as one byte-swapped
// store: the Vita's Cortex-A9 allows unaligned word and halfword stores to normal memory
// (-munaligned-access), so a word is REV + STR instead of four shifted byte stores. The bytes
// written are the same as before, in the same places.
static inline uint8_t* __PortGDWriteBE16(uint8_t* p, uint16_t value) {
    const uint16_t be = __builtin_bswap16(value);
    __builtin_memcpy(p, &be, 2);
    return p + 2;
}

static inline uint8_t* __PortGDWriteBE32(uint8_t* p, uint32_t value) {
    const uint32_t be = __builtin_bswap32(value);
    __builtin_memcpy(p, &be, 4);
    return p + 4;
}
