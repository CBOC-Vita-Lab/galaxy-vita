#include "../../platform/vgx/src/vgx_coldtrace.h"

#include <psp2/io/fcntl.h>
#include <cerrno>
#include <type_traits>

extern "C" {
decltype(sceIoOpen) __real_sceIoOpen;
void vgxvita_prepareShaderCacheWrite_v1(const char* path, int flags, SceMode mode);
void port_vgx_shader_bucket_open_guard_v1() {}
}

#if VGX_COLDTRACE
#include <psp2/gxm.h>
#include <psp2/io/stat.h>
#include <vitashark.h>

static_assert(VGX_COLDTRACE_DETAIL_VERSION==3);
static_assert(sizeof(SceOff)==8 && std::is_signed_v<SceOff> && std::is_signed_v<SceSSize>);
static_assert(sizeof(shark_type)==4 && sizeof(shark_opt)==4);
static_assert(sizeof(SceGxmOutputRegisterFormat)==4 && sizeof(SceGxmMultisampleMode)==4);

namespace {
using namespace vgx::coldtrace;
struct ErrnoShield {
    int saved=errno;
    void Before() noexcept {errno=saved;}
    void After() noexcept {saved=errno;}
    ~ErrnoShield() {errno=saved;}
};
template<class Call> auto Observe(Phase phase,IoClass io,uint64_t requested,Call call) {
    ErrnoShield error;
    Scope scope(g_recorder,phase,io);
    error.Before();
    if constexpr(std::is_void_v<decltype(call())>) {
        call();error.After();
    } else {
        const auto result=call();error.After();
        if constexpr(std::is_pointer_v<decltype(result)>)scope.Result(result?0:-1);
        else scope.Result(static_cast<int64_t>(result),requested);
        return result;
    }
}
}

extern "C" {
decltype(sceIoMkdir) __real_sceIoMkdir;
decltype(sceIoLseek) __real_sceIoLseek;
decltype(sceIoRead) __real_sceIoRead;
decltype(sceIoPread) __real_sceIoPread;
decltype(sceIoWrite) __real_sceIoWrite;
decltype(sceIoClose) __real_sceIoClose;
decltype(shark_compile_shader_extended) __real_shark_compile_shader_extended;
decltype(shark_compile_shader) __real_shark_compile_shader;
decltype(sceGxmShaderPatcherRegisterProgram) __real_sceGxmShaderPatcherRegisterProgram;
decltype(sceGxmShaderPatcherCreateVertexProgram) __real_sceGxmShaderPatcherCreateVertexProgram;
decltype(sceGxmShaderPatcherCreateFragmentProgram) __real_sceGxmShaderPatcherCreateFragmentProgram;
decltype(sceGxmBeginScene) __real_sceGxmBeginScene;
decltype(sceGxmEndScene) __real_sceGxmEndScene;
decltype(sceGxmFinish) __real_sceGxmFinish;
decltype(sceGxmNotificationWait) __real_sceGxmNotificationWait;
decltype(sceGxmTransferFinish) __real_sceGxmTransferFinish;

void port_vgx_coldtrace_wrapper_runtime_link_guard_v3() {}

int __wrap_sceIoMkdir(const char* path,SceMode mode) {
    return Observe(Phase::Mkdir,IoClass::Meta,0,[&]{return __real_sceIoMkdir(path,mode);});
}
SceOff __wrap_sceIoLseek(SceUID fd,SceOff offset,int whence) {
    return Observe(Phase::Seek,IoClass::Meta,0,[&]{return __real_sceIoLseek(fd,offset,whence);});
}
SceSSize __wrap_sceIoRead(SceUID fd,void* data,SceSize size) {
    return Observe(Phase::Read,IoClass::Read,size,[&]{return __real_sceIoRead(fd,data,size);});
}
int __wrap_sceIoPread(SceUID fd,void* data,SceSize size,SceOff offset) {
    return Observe(Phase::Read,IoClass::Read,size,[&]{return __real_sceIoPread(fd,data,size,offset);});
}
SceSSize __wrap_sceIoWrite(SceUID fd,const void* data,SceSize size) {
    return Observe(Phase::Write,IoClass::Write,size,[&]{return __real_sceIoWrite(fd,data,size);});
}
int __wrap_sceIoClose(SceUID fd) {
    return Observe(Phase::Close,IoClass::Meta,0,[&]{return __real_sceIoClose(fd);});
}
SceGxmProgram* __wrap_shark_compile_shader_extended(const char* src,uint32_t* size,shark_type type,shark_opt opt,int32_t fastmath,int32_t fastprecision,int32_t fastint) {
    return Observe(Phase::Shark,IoClass::None,0,[&]{return __real_shark_compile_shader_extended(src,size,type,opt,fastmath,fastprecision,fastint);});
}
SceGxmProgram* __wrap_shark_compile_shader(const char* src,uint32_t* size,shark_type type) {
    return Observe(Phase::Shark,IoClass::None,0,[&]{return __real_shark_compile_shader(src,size,type);});
}
int __wrap_sceGxmShaderPatcherRegisterProgram(SceGxmShaderPatcher* patcher,const SceGxmProgram* program,SceGxmShaderPatcherId* id) {
    return Observe(Phase::Register,IoClass::None,0,[&]{return __real_sceGxmShaderPatcherRegisterProgram(patcher,program,id);});
}
int __wrap_sceGxmShaderPatcherCreateVertexProgram(SceGxmShaderPatcher* patcher,SceGxmShaderPatcherId id,const SceGxmVertexAttribute* attributes,unsigned attributeCount,const SceGxmVertexStream* streams,unsigned streamCount,SceGxmVertexProgram** program) {
    return Observe(Phase::PatchVertex,IoClass::None,0,[&]{return __real_sceGxmShaderPatcherCreateVertexProgram(patcher,id,attributes,attributeCount,streams,streamCount,program);});
}
int __wrap_sceGxmShaderPatcherCreateFragmentProgram(SceGxmShaderPatcher* patcher,SceGxmShaderPatcherId id,SceGxmOutputRegisterFormat format,SceGxmMultisampleMode multisample,const SceGxmBlendInfo* blend,const SceGxmProgram* vertex,SceGxmFragmentProgram** program) {
    return Observe(Phase::PatchFragment,IoClass::None,0,[&]{return __real_sceGxmShaderPatcherCreateFragmentProgram(patcher,id,format,multisample,blend,vertex,program);});
}
int __wrap_sceGxmBeginScene(SceGxmContext* context,unsigned flags,const SceGxmRenderTarget* target,const SceGxmValidRegion* region,SceGxmSyncObject* vertexSync,SceGxmSyncObject* fragmentSync,const SceGxmColorSurface* color,const SceGxmDepthStencilSurface* depth) {
    return Observe(Phase::BeginScene,IoClass::None,0,[&]{return __real_sceGxmBeginScene(context,flags,target,region,vertexSync,fragmentSync,color,depth);});
}
int __wrap_sceGxmEndScene(SceGxmContext* context,const SceGxmNotification* vertex,const SceGxmNotification* fragment) {
    return Observe(Phase::EndScene,IoClass::None,0,[&]{return __real_sceGxmEndScene(context,vertex,fragment);});
}
void __wrap_sceGxmFinish(SceGxmContext* context) {
    Observe(Phase::Finish,IoClass::None,0,[&]{__real_sceGxmFinish(context);});
}
int __wrap_sceGxmNotificationWait(const SceGxmNotification* notification) {
    return Observe(Phase::Notification,IoClass::None,0,[&]{return __real_sceGxmNotificationWait(notification);});
}
int __wrap_sceGxmTransferFinish(void) {
    return Observe(Phase::TransferFinish,IoClass::None,0,[&]{return __real_sceGxmTransferFinish();});
}
}

#define CHECK_ABI(symbol) static_assert(std::is_same_v<decltype(&__wrap_##symbol),decltype(&symbol)>)
CHECK_ABI(sceIoMkdir);CHECK_ABI(sceIoLseek);CHECK_ABI(sceIoRead);
CHECK_ABI(sceIoPread);CHECK_ABI(sceIoWrite);CHECK_ABI(sceIoClose);
CHECK_ABI(shark_compile_shader_extended);CHECK_ABI(shark_compile_shader);
CHECK_ABI(sceGxmShaderPatcherRegisterProgram);CHECK_ABI(sceGxmShaderPatcherCreateVertexProgram);CHECK_ABI(sceGxmShaderPatcherCreateFragmentProgram);
CHECK_ABI(sceGxmBeginScene);CHECK_ABI(sceGxmEndScene);CHECK_ABI(sceGxmFinish);CHECK_ABI(sceGxmNotificationWait);
CHECK_ABI(sceGxmTransferFinish);
#undef CHECK_ABI
#endif

// Functional in both builds: only vitaGL's exact private-cache write shape needs
// preparation. Other opens avoid even errno/TLS access in the normal build.
namespace {
void PrepareCacheWrite(const char* path, int flags, SceMode mode) {
    const int saved = errno;
    vgxvita_prepareShaderCacheWrite_v1(path, flags, mode);
    errno = saved;
}
#if !VGX_COLDTRACE
// Keep argument saves and errno access out of the ordinary open/read dispatcher.
__attribute__((noinline)) SceUID OpenPrivateShaderCache(const char* path, int flags, SceMode mode) {
    PrepareCacheWrite(path, flags, mode);
    return __real_sceIoOpen(path, flags, mode);
}
#endif
}
extern "C" SceUID __wrap_sceIoOpen(const char* path, int flags, SceMode mode) {
    static_assert((SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC) == 0x602);
    if (flags == (SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC) && mode == 0777) {
#if VGX_COLDTRACE
        PrepareCacheWrite(path, flags, mode);
#else
        return OpenPrivateShaderCache(path, flags, mode);
#endif
    }
#if VGX_COLDTRACE
    return Observe(Phase::Open, IoClass::Meta, 0, [&] { return __real_sceIoOpen(path, flags, mode); });
#else
    return __real_sceIoOpen(path, flags, mode);
#endif
}
static_assert(std::is_same_v<decltype(&__wrap_sceIoOpen), decltype(&sceIoOpen)>);
