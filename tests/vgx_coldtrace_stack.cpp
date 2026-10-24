#include "../platform/vgx/src/vgx_coldtrace.h"
#if VGX_COLDTRACE
extern "C" __attribute__((noinline)) bool coldtrace_arm(vgx::coldtrace::Recorder::Clock clock,vgx::coldtrace::Recorder::ThreadId thread) {
    return vgx::coldtrace::g_recorder.Arm(clock,thread,1);
}
extern "C" __attribute__((noinline)) uint64_t coldtrace_overlap(const vgx::coldtrace::FrozenView& view,const vgx::coldtrace::Item& item) {
    return vgx::coldtrace::Overlap(view,item,vgx::coldtrace::IoClass::Any).us;
}
extern "C" __attribute__((noinline)) void coldtrace_item() {
    vgx::coldtrace::Scope item(vgx::coldtrace::g_recorder,vgx::coldtrace::Key{});
    vgx::coldtrace::Scope phase(vgx::coldtrace::g_recorder,vgx::coldtrace::Phase::Compile);
}
extern "C" __attribute__((noinline)) void coldtrace_worker() {
    vgx::coldtrace::Scope io(vgx::coldtrace::g_recorder,vgx::coldtrace::Phase::Read,vgx::coldtrace::IoClass::Read);
    io.Result(64,64);
}
#endif
