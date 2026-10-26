#pragma once
#include <cstdint>

// Host fixture declarations only; this does not validate the Vita SDK ABI.
struct SceGxmNotification {
    volatile unsigned int* address;
    std::uint32_t value;
};
