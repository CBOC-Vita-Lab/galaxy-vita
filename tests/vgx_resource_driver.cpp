// Extracted production logger plus the real quiet-player basic-log adapter.
#include "../platform/vgx/src/vgx_autoq.cpp"
#include "../platform/vgx/src/vgx_resource_audit.h"
#include <string>
#include <vector>
#include <unistd.h>
namespace {
std::vector<std::string> messages;
unsigned ordinaryCalls = 0;
void Require(bool ok, const char* reason) {
    if (ok) return;
    ::write(2, reason, std::strlen(reason)); ::write(2, "\n", 1); std::exit(1);
}
bool Contains(const char* text) {
    for (const auto& message : messages) if (message.find(text) != std::string::npos) return true;
    return false;
}
}
extern "C" {
SceGxmNotification query_fence{nullptr,0};
std::int64_t port_thread_cpu_us() { return 0; }
unsigned port_vgx_pace_divisor() { return 0; }
void vglRtSetup(std::uint32_t, int, int) { std::abort(); }
void vglRtSetRingGuard(std::uint32_t) { std::abort(); }
std::uint32_t vglRtRingDrops() { std::abort(); }
std::uint32_t vglRtLastGpuFrame(VgxRtGpuFrame*) { std::abort(); }
void vglRtGpuCounters(std::uint32_t*,std::uint32_t*,std::uint32_t*,std::uint32_t*) { std::abort(); }
void port_log(const char*, ...) { ++ordinaryCalls; }
void port_log_summary(const char* format, ...) {
    char text[512]; va_list args; va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args); va_end(args); messages.emplace_back(text);
}
std::uint64_t sceKernelGetProcessTimeWide() { return 0; }
SceUID sceKernelCreateSema(const char*, unsigned, int, int, const void*) { return -1; }
int sceKernelWaitSema(SceUID, int, SceUInt*) { std::abort(); }
int sceKernelSignalSema(SceUID, int) { std::abort(); }
SceUID sceKernelCreateThread(const char*, int (*)(SceSize, void*), int, SceSize, unsigned, int, const void*) { std::abort(); }
int sceKernelStartThread(SceUID, SceSize, void*) { std::abort(); }
int sceKernelDelayThread(SceUInt) { std::abort(); }
}
#include "vgx_driver_logger.inc"
int main(int argc, char** argv) {
    Require(argc == 2, "supply expected audit state");
    const bool audit = std::strcmp(argv[1], "1") == 0;
    for (unsigned i=0;i<600;++i) msc_vgl_printf("ordinary message %u\n",i);
    for (unsigned i=0;i<20;++i) msc_vgl_printf("Shader Compiler: W] unreferenced local\n");
    Require(messages.empty(), "ordinary and compiler warning messages never enter the basic audit log");
    msc_vgl_printf("Shader Compiler: E] compile failed on line 3.\n");
    msc_vgl_printf("Fragment shader patching failed (SCE_GXM_ERROR_OUT_OF_MEMORY) on shader 0x5.\n");
    msc_vgl_printf("gpu_alloc_mapped_temp failed with a requested size of 1024 bytes.\n");
    Require(!audit || (Contains("category=compiler") && Contains("category=gxm") && Contains("category=allocation")),
            "rare native errors survive quiet-player logging after the ordinary 400-message cap");
    msc_vgl_printf("[VGL_UNIFORM_LIFETIME] fatal: sceGxmBeginScene=%d\n", -11);
    msc_vgl_printf("[VGL_UNIFORM_LIFETIME] fatal: sceGxmEndScene=%d\n", -12);
    msc_vgl_printf("[VGL_UNIFORM_LIFETIME] fatal: cache/request exceed backing capacity\n");
    msc_vgl_printf("[VGL_UNIFORM_LIFETIME] fatal: oversized request=%u\n", 1048576u);
    Require(!audit || (Contains("category=gxm record=2 info=[VGL_UNIFORM_LIFETIME] fatal: sceGxmBeginScene=-11") &&
                       Contains("category=gxm record=3 info=[VGL_UNIFORM_LIFETIME] fatal: sceGxmEndScene=-12")),
            "numeric native scene fatals survive quiet-player logging");
    Require(!audit || (Contains("category=allocation record=2 info=[VGL_UNIFORM_LIFETIME] fatal: cache/request exceed backing capacity") &&
                       Contains("category=allocation record=3 info=[VGL_UNIFORM_LIFETIME] fatal: oversized request=1048576")),
            "native uniform backing capacity fatals survive quiet-player logging");
    setenv("GXV_RESOURCE_AUDIT",audit ? "0" : "1",1);
    for (unsigned i=0;i<4096;++i) msc_vgl_printf("Fragment shader patching failed (SCE_GXM_ERROR_OUT_OF_MEMORY) on shader 0x%X.\n",i);
    Require(audit ? messages.size()<64 && Contains("suppressed=") : messages.empty(),
            "native audit is read once and bounded independently of ordinary logs");
    for (const auto& message: messages) Require(message.size()<512 && message.find('\n')==std::string::npos,"audit lines are bounded single lines");
    char report[160]; const int n=std::snprintf(report,sizeof report,"PASS native quiet logger audit=%d basic_lines=%zu ordinary_calls=%u\n",audit,messages.size(),ordinaryCalls);
    ::write(1,report,n);
}
