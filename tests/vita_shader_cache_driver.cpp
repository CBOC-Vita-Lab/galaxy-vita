// Keep SDK definitions in a separate object so the real link-time wrappers are exercised.
#include "vita_shader_cache_audit.h"
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

extern "C" {
#ifndef CACHE_WEAK_ROOT
char vgl_shader_cache_path[256] = {};
#endif
shader* shaders[8] = {};
bool is_shark_online = true;
vglSemanticMode glsl_sema_mode = VGL_MODE_GLOBAL;
shark_opt compiler_opts = SHARK_OPT_FAST;
int32_t compiler_fastmath = 0, compiler_fastprecision = 0, compiler_fastint = 0;
SceGxmShaderPatcher* gxm_shader_patcher = nullptr;
char* shark_log = nullptr;
void port_log(const char*, ...) {}
SceUInt64 sceKernelGetProcessTimeWide() { return audit::Clock(); }
void port_vgx_coldtrace_runtime_link_guard_v3() {}
GLuint glCreateShader(GLenum type) {
    const auto id = audit::nextShader++;
    audit::Require(id <= 8, "bounded fixture shader handles");
    shaders[id - 1] = static_cast<shader*>(std::calloc(1, sizeof(shader)));
    shaders[id - 1]->type = type; shaders[id - 1]->is_glsl = true; return id;
}
void glShaderSource(GLuint id, GLsizei count, const GLchar* const* parts, const GLint* lengths) {
    audit::Require(id == audit::expectedShader && count == audit::expectedCount && parts == audit::expectedStrings && lengths == audit::expectedLengths,
                   "host forwards source pointers, lengths, count and shader unchanged");
    ++audit::nativeSources;
    const bool saved = audit::countHostAllocation; audit::countHostAllocation = false;
    frozen_glShaderSource(id, count, parts, lengths);
    audit::countHostAllocation = saved;
}
void* vglMalloc(uint32_t size) { return std::malloc(size); }
void vgl_free(void* p) { std::free(p); }
void* vgl_fast_memcpy(void* destination, const void* source, size_t bytes) { return std::memcpy(destination, source, bytes); }
void vgl_log(const char*, ...) {}
const char* get_gxm_error_literal(int) { return "fixture"; }
bool start_shader_compiler() { return true; }
void glsl_translator_process(shader*) {}
void shader_fixture_gl_error(GLenum) { audit::Require(false, "unexpected native GL error"); }
SceGxmProgram* shark_compile_shader_extended(const char* source, uint32_t* size, shark_type type, shark_opt opt, int32_t fm, int32_t fp, int32_t fi) {
    audit::Require(opt == SHARK_OPT_FAST && fm == 0 && fp == 0 && fi == 0, "compiler settings unchanged");
    ++audit::compilerCalls;
    const uint64_t hash = XXH3_64bits(source, *size);
    audit::binary.fill(0x35); std::memcpy(audit::binary.data(), &hash, sizeof(hash)); audit::binary[8] = static_cast<uint8_t>(type);
    *size = 24; errno = EAGAIN;
    return reinterpret_cast<SceGxmProgram*>(audit::binary.data());
}
void shark_clear_output() {}
const SceShaccCgCompileOutput* shark_get_internal_compile_output() { return nullptr; }
SceShaccCgParameter sceShaccCgGetFirstParameter(const SceShaccCgCompileOutput*) { return nullptr; }
SceShaccCgParameter sceShaccCgGetNextParameter(SceShaccCgParameter) { return nullptr; }
SceShaccCgParameterClass sceShaccCgGetParameterClass(SceShaccCgParameter) { return SCE_SHACCCG_PARAMETERCLASS_INVALID; }
const char* sceShaccCgGetParameterName(SceShaccCgParameter) { return ""; }
uint32_t sceShaccCgGetParameterBufferIndex(SceShaccCgParameter) { return 0; }
uint32_t* vglProgramGetParameterBase(const SceGxmProgram*) { audit::Require(false, "fixture declares no matrix uniforms"); return nullptr; }
int sceGxmShaderPatcherRegisterProgram(SceGxmShaderPatcher*, const SceGxmProgram* program, SceGxmShaderPatcherId* id) { *id = reinterpret_cast<SceGxmShaderPatcherId>(const_cast<SceGxmProgram*>(program)); return 0; }
unsigned sceGxmProgramGetDefaultUniformBufferSize(const SceGxmProgram*) { return 0; }
unsigned sceGxmProgramParameterGetIndex(const SceGxmProgram*, const SceGxmProgramParameter*) { return 0; }
const SceGxmProgramParameter* sceGxmProgramFindParameterByName(const SceGxmProgram*, const char*) { return nullptr; }
int sceIoMkdir(const char* path, SceMode mode) {
    ++audit::mkdirCalls;
    audit::nowUs.fetch_add(10);
    std::lock_guard<std::mutex> lock(audit::sdkMutex); audit::lastMkdirPath = path;
    if (audit::mkdirFailure > 0) { --audit::mkdirFailure; errno = EINTR; return audit::SdkError(EIO); }
    const int result = ::mkdir(path, static_cast<mode_t>(mode));
    const int error = errno; errno = EINTR;
    return result < 0 ? audit::SdkError(error) : 0;
}
SceUID sceIoOpen(const char* path, int flags, SceMode mode) {
    ++audit::openCalls;
    audit::nowUs.fetch_add(10);
    std::lock_guard<std::mutex> lock(audit::sdkMutex);
    audit::openIncomingErrno = errno; audit::lastOpenPath = path; audit::lastFlags = flags; audit::lastMode = mode;
    if ((flags & SCE_O_WRONLY) && audit::openFailure > 0) { --audit::openFailure; return audit::SdkError(EROFS); }
    int native = (flags & SCE_O_RDWR) == SCE_O_RDWR ? O_RDWR : (flags & SCE_O_WRONLY) ? O_WRONLY : O_RDONLY;
    if (flags & SCE_O_CREAT) native |= O_CREAT;
    if (flags & SCE_O_TRUNC) native |= O_TRUNC;
    if (flags & SCE_O_APPEND) native |= O_APPEND;
    const int fd = ::open(path, native, static_cast<mode_t>(mode));
    const int result = fd < 0 ? audit::SdkError(errno) : fd;
    if (audit::openErrno > 0) errno = audit::openErrno;
    return result;
}
SceOff sceIoLseek(SceUID fd, SceOff offset, int whence) { ++audit::seekCalls; audit::nowUs.fetch_add(10); return ::lseek(fd, offset, whence); }
SceSSize sceIoRead(SceUID fd, void* data, SceSize size) { ++audit::readCalls; audit::nowUs.fetch_add(10); return ::read(fd, data, size); }
SceSSize sceIoWrite(SceUID fd, const void* data, SceSize size) { ++audit::writeCalls; audit::nowUs.fetch_add(10); if (fd < 0) { ++audit::sdkErrors; return fd; } return ::write(fd, data, size); }
int sceIoClose(SceUID fd) { ++audit::closeCalls; audit::nowUs.fetch_add(10); if (fd < 0) { ++audit::sdkErrors; return fd; } return ::close(fd); }
}
