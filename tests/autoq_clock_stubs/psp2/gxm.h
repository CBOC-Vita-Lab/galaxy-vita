#pragma once
#include <cstdint>

// Host fixture declarations only; this does not validate the Vita SDK ABI.
struct SceGxmNotification {
    volatile unsigned int* address;
    std::uint32_t value;
};

// (GXV_MIDFLUSH in vgx_backend_gl.cpp) the context and program types and the mid-scene flush, as names only
struct SceGxmContext;
struct SceGxmFragmentProgram;
struct SceGxmVertexProgram;
struct SceGxmSyncObject;
enum { SCE_GXM_MIDSCENE_PRESERVE_DEFAULT_UNIFORM_BUFFERS = 0x00000001u };
// (midflush31) the test's handle on the flush: its return value (a failure), how many were made, and a hook run at each one
inline int g_stubMidRc = 0, g_stubMidCalls = 0;
inline void (*g_stubMidHook)() = nullptr;
inline int sceGxmMidSceneFlush(SceGxmContext*, unsigned int, SceGxmSyncObject*, const SceGxmNotification*) {
    if (g_stubMidRc >= 0) { ++g_stubMidCalls; if (g_stubMidHook) g_stubMidHook(); }
    return g_stubMidRc;
}
