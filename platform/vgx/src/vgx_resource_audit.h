#pragma once
#include "vgx_common.h"
#include "vgx_autoq.h"
#include <atomic>
#include <cstring>
#include "../../include/port/hot_knob.h"

namespace vgx::resourceaudit {
// GXV_RESOURCE_AUDIT=1: opt-in basic-log evidence, including quiet player builds.
// Read once, default off; never changes resource, retry or render policy.
inline bool Enabled() {
    return PORT_HOT_KNOB(bool, EnvInt("GXV_RESOURCE_AUDIT", 0) == 1);  // (GXV_HOT_KNOBS: read without a barrier, port/hot_knob.h)
}

inline void DriverMessage(const char* text) {
    if (!Enabled()) return;
    unsigned category;
    if (std::strstr(text, "Shader Compiler: E]") || std::strstr(text, "error occurred during GLSL translation") ||
        std::strstr(text, "Too many samplers used")) category = 0;
    else if (std::strstr(text, "shader patching failed") || std::strstr(text, "Program registration failed") ||
             std::strstr(text, "sceGxmBeginScene erroring") || std::strstr(text, "Failed to create a rendertarget") ||
             std::strstr(text, "Failed to create a shared rendertarget") ||
             std::strstr(text, "[VGL_UNIFORM_LIFETIME] fatal: sceGxmBeginScene=") ||
             std::strstr(text, "[VGL_UNIFORM_LIFETIME] fatal: sceGxmEndScene=")) category = 1;
    else if (std::strstr(text, "failed to alloc") || std::strstr(text, "failed allocating") ||
             std::strstr(text, "allocation failed") || std::strstr(text, "failed reallocating") ||
             std::strstr(text, "Out of memory") || (std::strstr(text, "gpu_alloc_") && std::strstr(text, "failed")) ||
             std::strstr(text, "[VGL_UNIFORM_LIFETIME] fatal: cache/request exceed backing capacity") ||
             std::strstr(text, "[VGL_UNIFORM_LIFETIME] fatal: oversized request=")) category = 2;
    else return;
    // Driver callbacks need not all originate on the renderer. Counters are
    // independent of normal log throttles and saturate instead of wrapping.
    static std::atomic<uint32_t> seen[3]{};
    uint32_t old = seen[category].load(std::memory_order_relaxed);
    do { if (old == ~uint32_t{0}) return; }
    while (!seen[category].compare_exchange_weak(old, old + 1, std::memory_order_relaxed));
    static const char* names[] = {"compiler", "gxm", "allocation"};
    const uint32_t count = old + 1;
    if (count > 16) {
        const uint32_t suppressed = count - 16;
        if (!(suppressed & (suppressed - 1)))
            autoq::Log("[resource-audit] stage=native-driver category=%s emitted=16 suppressed=%u", names[category], suppressed);
        return;
    }
    char excerpt[257] = {};
    for (unsigned i=0; i<sizeof excerpt-1 && text[i]; ++i) excerpt[i] = text[i] < ' ' ? ' ' : text[i];
    autoq::Log("[resource-audit] stage=native-driver category=%s record=%u info=%s", names[category], count, excerpt);
}
} // namespace vgx::resourceaudit
