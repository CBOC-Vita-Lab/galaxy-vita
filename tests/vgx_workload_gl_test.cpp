// Reuse the real backend's existing pixel/target suite; also count observer-sensitive GL calls.
#define main ExistingAutoQualityMain
#include "vgx_autoq_gl_test.cpp"
#undef main
#include "vgx_workload.h"
#include <chrono>
#include <time.h>

static unsigned FinishCalls = 0, ReadCalls = 0;
extern "C" void __real_glFinish();
extern "C" void __wrap_glFinish() { ++FinishCalls; __real_glFinish(); }
extern "C" void __real_glReadPixels(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
extern "C" void __wrap_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* data) {
    ++ReadCalls; __real_glReadPixels(x, y, w, h, format, type, data);
}
extern "C" int64_t port_host_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" int64_t port_thread_cpu_us() {
    timespec t{};
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) == 0 ? static_cast<int64_t>(t.tv_sec) * 1000000 + t.tv_nsec / 1000 : 0;
}
extern "C" void port_log_summary(const char*, ...) {}
int main(int argc, char**) {
    const bool on = argc > 1;
    setenv("GXV_WORKLOAD_PROBE", on ? "1" : "0", 1);
    const int status = ExistingAutoQualityMain();
    if (status) return status;
    using namespace vgx::workload;
    if (on && (!Pictures.pictures || !Pictures.textureCopies || !Pictures.displayCopies || !Pictures.backendScenes)) {
        std::fprintf(stderr, "FAIL: real backend did not dispatch workload census\n"); return 1;
    }
    if (!on && Pictures.pictures) { std::fprintf(stderr, "FAIL: disabled backend recorded census\n"); return 1; }
    std::printf("workload GL calls: finish=%u readpixels=%u\n", FinishCalls, ReadCalls);
    std::printf("PASS: real backend workload probe %s; existing target/pixel checks passed\n", on ? "on" : "off");
}
