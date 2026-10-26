#pragma once

#include <atomic>

namespace port::bootdialog {

inline std::atomic<bool> g_enabled{false};
inline std::atomic<bool> g_requested{false};

inline void Enable(bool enabled) { g_enabled.store(enabled, std::memory_order_release); }
inline bool Enabled() { return g_enabled.load(std::memory_order_acquire); }
inline void Drawn() {
    if (Enabled()) g_requested.store(true, std::memory_order_release);
}
inline bool Requested() { return g_requested.load(std::memory_order_acquire); }

}  // namespace port::bootdialog
