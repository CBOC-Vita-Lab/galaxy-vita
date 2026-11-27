#pragma once
#include <cstdint>

namespace port {
inline bool TourScenarioNumber(const char* text, int32_t& out) {
    uint32_t n = 0;
    if (!text || !*text) return false;
    for (; *text; ++text) {
        const unsigned digit = static_cast<unsigned>(*text - '0');
        if (digit > 9 || n > (0x7fffffffu - digit) / 10) return false;
        n = n * 10 + digit;
    }
    if (!n) return false;
    out = static_cast<int32_t>(n);
    return true;
}
} // namespace port
