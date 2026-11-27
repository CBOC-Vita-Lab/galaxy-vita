// Reuse existing SDK/reflection/host accounting around the actual extracted driver.
#define main original_shader_cache_test_main
#include "vita_shader_cache.cpp"
#undef main
#include "../platform/vgx/vita/vgx_shader_pack.h"

namespace {
bool failCopy = false;
bool failHostPack = false;
unsigned copyFailures = 0;
unsigned hostPackFailures = 0;
size_t failCandidateSize = 0;
bool failCheck = false, failRegistration = false;
bool tinyLooseReads = false, failLooseRead = false, eofLooseRead = false, failLooseClose = false;
unsigned rejectedCandidates = 0, allocated = 0, released = 0;
std::atomic<unsigned> startupSummaries{0}, lookupSummaries{0};
shader normalized{};
std::array<uint8_t, 176> program{};
bool haveReference = false;
bool checkLateLookup = false;
char latePath[320] = {};
void LateLookup() {
    if (!checkLateLookup) return;
    const void* data = nullptr; size_t size = 0;
    audit::Require(g_shaderPack.Find(latePath, &data, &size) && size == 184,
                   "process-owned cache remains valid after ordinary C++ static destruction");
    std::puts("PASS: shader-pack lookup remains valid during late process teardown");
}
__attribute__((constructor(101))) void RegisterLateLookup() { std::atexit(LateLookup); }
void StageFile(const std::string& path, char stage = 'v') {
    std::vector<uint8_t> bytes(184, 0);
    bytes[0] = 1; // One restored matrix uniform, parameter index zero.
    std::memcpy(bytes.data() + 8, "GXP", 4);
    bytes[16] = 176; bytes[28] = stage == 'f'; bytes[44] = 1; bytes[48] = 116;
    bytes[164] = 16; bytes[180] = 'm'; // Relative parameter name within the declared GXP image.
    audit::Put(path, bytes);
}
void CheckShader(GLuint id) {
    auto& s = *shaders[id - 1];
    audit::Require(s.prog && s.id == reinterpret_cast<SceGxmShaderPatcherId>(const_cast<SceGxmProgram*>(s.prog)) &&
        s.source == nullptr && s.size == 176 && s.mat && s.mat->chain == nullptr &&
        s.mat->ptr == reinterpret_cast<const SceGxmProgramParameter*>(reinterpret_cast<const uint8_t*>(s.prog) + 156),
        "driver restores source lifetime, registration, program size and exact matrix-parameter metadata");
    shader state = s;
    state.id = nullptr; state.prog = nullptr; state.mat = nullptr;
    if (!haveReference) {
        normalized = state; std::memcpy(program.data(), s.prog, program.size()); haveReference = true;
    } else audit::Require(normalized.type == state.type && normalized.slot == state.slot && normalized.dirty == state.dirty &&
        normalized.is_glsl == state.is_glsl && normalized.ref_counter == state.ref_counter && normalized.size == state.size &&
        normalized.unif_buf_size == state.unif_buf_size && normalized.unif_blk == state.unif_blk && normalized.log == state.log &&
        std::memcmp(normalized.semantics.texcoord_names, state.semantics.texcoord_names, sizeof state.semantics.texcoord_names) == 0 &&
        std::memcmp(normalized.semantics.color_names, state.semantics.color_names, sizeof state.semantics.color_names) == 0 &&
        std::memcmp(normalized.semantics.texcoord_used, state.semantics.texcoord_used, sizeof state.semantics.texcoord_used) == 0 &&
        std::memcmp(normalized.semantics.color_used, state.semantics.color_used, sizeof state.semantics.color_used) == 0 &&
        std::memcmp(program.data(), s.prog, program.size()) == 0,
                         "complete non-owning shader state and binary match the original file path");
    std::free(s.mat); s.mat = nullptr;
    audit::Destroy(id);
}
void LoadOne(bool ram) {
    audit::ResetCounts();
    allocated = released = 0;
    const auto id = audit::Create(GL_VERTEX_SHADER);
    const char* text = "abc"; audit::Source(id, 1, &text, nullptr);
    glCompileShader(id);
    audit::Require(audit::compilerCalls == 0 && audit::nativeSources == 1 && audit::mkdirCalls == 0 && audit::writeCalls == 0,
                   "cached load preserves source forwarding and never compiles or writes");
    audit::Require(audit::openCalls == (ram ? 0u : 1u) && audit::seekCalls == (ram ? 0u : 2u) &&
                   audit::readCalls == (ram ? 0u : tinyLooseReads ? 62u : 1u) && audit::closeCalls == (ram ? 0u : 1u),
                   "RAM hit removes five file calls; ordinary fallback preserves every original call");
    audit::Require(allocated == released + 2, "failed RAM candidates and temporary copies leave only committed program/matrix ownership");
    CheckShader(id);
}
void CompileMiss() {
    const char* text = "new-unpacked-source";
    audit::fs::remove(audit::CachePath(audit::root, 'v', XXH3_64bits(text, std::strlen(text))));
    audit::ResetCounts();
    const auto id = audit::Create(GL_VERTEX_SHADER);
    audit::Source(id, 1, &text, nullptr);
    glCompileShader(id);
    audit::Require(audit::compilerCalls == 1 && shaders[id - 1]->prog && !shaders[id - 1]->source && shaders[id - 1]->size == 176,
                   "unpacked source retains the actual hardened compiler/commit path");
    audit::Require(audit::openCalls == 2 && audit::writeCalls == 1 && audit::closeCalls == 1,
                   "unpacked successful compile preserves original temporary-file persistence");
    audit::Destroy(id);
}
void LooseFailure() {
    audit::ResetCounts(); allocated = released = 0;
    const auto id = audit::Create(GL_VERTEX_SHADER);
    const char* text = "abc"; audit::Source(id, 1, &text, nullptr);
    glCompileShader(id);
    audit::Require(audit::compilerCalls == 1 && shaders[id - 1]->prog && !shaders[id - 1]->source && shaders[id - 1]->size == 176,
                   "actual loose reader failure retains source for compiler fallback and successful commit");
    audit::Require(allocated == released + 1 && audit::readCalls == 1 && audit::closeCalls == 2,
                   "failed loose read/close cleans temporary bytes and preserves original compiler persistence");
    audit::Destroy(id);
}
}
extern "C" {
void port_log_summary(const char* format, ...) {
    if (std::strstr(format, "shader RAM pack:")) ++startupSummaries;
    if (std::strstr(format, "shader RAM pack lookup:")) ++lookupSummaries;
}
void* __real_malloc(size_t);
void* __wrap_malloc(size_t size) {
    if (failHostPack && size == 456) { failHostPack = false; ++hostPackFailures; return nullptr; }
    return __real_malloc(size);
}
void* __real_vglMalloc(uint32_t);
void* __wrap_vglMalloc(uint32_t size) {
    if (failCopy && size == 184) {
        audit::Require(shaders[audit::expectedShader - 1]->source && std::strcmp(shaders[audit::expectedShader - 1]->source, "abc") == 0,
                       "copy OOM occurs before the original source is freed or changed");
        failCopy = false; ++copyFailures; return nullptr;
    }
    if (failCandidateSize && size == failCandidateSize) {
        audit::Require(shaders[audit::expectedShader - 1]->source && shaders[audit::expectedShader - 1]->size == 3,
                       "candidate OOM preserves source and source byte length before loose fallback");
        failCandidateSize = 0; ++rejectedCandidates; return nullptr;
    }
    void* p = __real_vglMalloc(size);
    if (p) ++allocated;
    return p;
}
void __real_vgl_free(void*);
void __wrap_vgl_free(void* p) { if (p) ++released; __real_vgl_free(p); }
ssize_t __real_read(int, void*, size_t);
ssize_t __wrap_read(int fd, void* data, size_t size) {
    if (failLooseRead) { failLooseRead = false; errno = EIO; return -1; }
    if (eofLooseRead) { eofLooseRead = false; return 0; }
    return __real_read(fd, data, tinyLooseReads ? std::min(size, size_t(3)) : size);
}
int __real_close(int);
int __wrap_close(int fd) {
    const int result = __real_close(fd);
    if (failLooseClose) { failLooseClose = false; errno = EIO; return -1; }
    return result;
}
int sceGxmProgramCheck(const SceGxmProgram*) {
    if (failCheck) { failCheck = false; ++rejectedCandidates; return -1; }
    return 0; // SDK double: actual hardened byte/parameter validation still runs.
}
unsigned sceGxmProgramGetParameterCount(const SceGxmProgram* p) {
    uint32_t count; std::memcpy(&count, reinterpret_cast<const uint8_t*>(p) + 36, 4); return count;
}
int fixture_sceGxmShaderPatcherRegisterProgram(SceGxmShaderPatcher*, const SceGxmProgram*, SceGxmShaderPatcherId*);
int sceGxmShaderPatcherRegisterProgram(SceGxmShaderPatcher* patcher, const SceGxmProgram* p, SceGxmShaderPatcherId* id) {
    if (failRegistration) {
        audit::Require(shaders[audit::expectedShader - 1]->source && shaders[audit::expectedShader - 1]->size == 3,
                       "registration failure cannot consume source or publish candidate state");
        failRegistration = false; ++rejectedCandidates; return -1;
    }
    return fixture_sceGxmShaderPatcherRegisterProgram(patcher, p, id);
}
int sceIoRename(const char* a, const char* b) { return std::rename(a, b); }
int sceIoRemove(const char* a) { return std::remove(a); }
SceGxmProgram* shark_compile_shader_extended(const char* source, uint32_t* size, shark_type, shark_opt, int32_t, int32_t, int32_t) {
    audit::Require(*size == std::strlen(source), "compiler receives original source byte length after RAM rejection/miss");
    ++audit::compilerCalls;
    *size = program.size();
    return reinterpret_cast<SceGxmProgram*>(program.data());
}
uint32_t* __wrap_vglProgramGetParameterBase(const SceGxmProgram* program) {
    // Synthetic parameter table at the declared +156. No native GXP validation claim.
    return reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(const_cast<SceGxmProgram*>(program)) + 156);
}
}
int main(int argc, char** argv) {
    audit::Require(argc == 3, "supply prepare|run|off|oom and fixture directory");
    const std::string mode = argv[1];
    const char* key = std::strrchr(ShaderCacheFolder(), '/');
    audit::Require(key != nullptr, "real shader cache exposes its ABI folder");
    const std::string folder = std::string(argv[2]) + key;
    audit::root = folder + "/v3";
    if (mode == "prepare") {
        audit::fs::remove_all(argv[2]); audit::SetRoot(audit::root);
        for (char stage : {'v', 'f'}) StageFile(audit::CachePath(audit::root, stage, 0x78af5f94892f3950ull), stage);
        char identity[512]; ShaderPackIdentity(identity, sizeof identity);
        std::puts(identity); return 0;
    }
    audit::SetRoot(audit::root);
    if (mode == "bad-ram") StageFile(audit::CachePath(audit::root, 'v', 0x78af5f94892f3950ull));
    LoadOne(false); // Independent baseline: new driver, pack not loaded yet.
    if (mode == "bad-ram") {
        auto bytes = audit::Bytes(folder + "/stages.pack");
        uint64_t offset = 0; std::memcpy(&offset, bytes.data() + 56, 8);
        const uint32_t invalidRelativeName = 0xffffff00u;
        std::memcpy(bytes.data() + offset + 8 + 156, &invalidRelativeName, 4);
        const uint64_t checksum = XXH3_64bits(bytes.data() + 40, bytes.size() - 40);
        std::memcpy(bytes.data() + 32, &checksum, 8);
        audit::Put(folder + "/stages.pack", bytes);
    }
    if (mode == "oom") failHostPack = true;
    PrepareShaderPack(folder.c_str());
    audit::Require(startupSummaries == 1 && lookupSummaries == 0, "one basic startup availability summary does not imply a RAM lookup");
    if (mode == "bad-ram") {
        audit::Require(g_shaderPack.Entries() == 2, "basic pack validator deliberately does not replace hardened parameter-name validation");
        LoadOne(false);
        CompileMiss();
    } else if (mode == "oom") {
        audit::Require(g_shaderPack.Bytes() == 0 && !failHostPack && hostPackFailures == 1,
                       "actual host preload allocation failure publishes nothing and leaves ordinary cache available");
        LoadOne(false);
    } else if (mode == "off") {
        audit::Require(g_shaderPack.Bytes() == 0, "default-off does not read or allocate the pack");
        setenv("GXV_SHADERPACK", "1", 1); PrepareShaderPack(folder.c_str());
        audit::Require(g_shaderPack.Bytes() == 0, "configuration cannot be re-enabled after the one-shot startup decision");
        LoadOne(false);
    } else {
        audit::Require(mode == "run" && g_shaderPack.Entries() == 2 && g_shaderPack.Bytes() == 456,
                       "real startup host loads the independently built, full-ABI-bound pack");
        std::snprintf(latePath, sizeof latePath, "%s/v/78/78AF5F94892F3950.gxp", audit::root.c_str());
        checkLateLookup = true;
        std::atomic<bool> concurrentOk{true};
        std::thread readers[4];
        for (auto& reader : readers) reader = std::thread([&] {
            for (unsigned n = 0; n < 1000; ++n) {
                const void* data = nullptr; size_t size = 0;
                if (!vgxvita_shaderCacheRead_v1(latePath, &data, &size) || !data || size != 184) concurrentOk = false;
            }
        });
        for (auto& reader : readers) reader.join();
        audit::Require(concurrentOk && lookupSummaries == 1, "actual immutable hook attests exactly one first lookup across concurrent readers");
        for (unsigned i = 0; i < 64; ++i) LoadOne(true);
        glsl_sema_mode = VGL_MODE_SHADER_PAIR; LoadOne(false);
        glsl_sema_mode = VGL_MODE_GLOBAL; LoadOne(true);
        glsl_sema_mode = VGL_MODE_POSTPONED;
        audit::ResetCounts();
        const auto deferred = audit::Create(GL_VERTEX_SHADER); const char* text = "abc";
        audit::Source(deferred, 1, &text, nullptr); glCompileShader(deferred);
        audit::Require(shaders[deferred - 1]->source && !shaders[deferred - 1]->prog && audit::openCalls == 0 && audit::compilerCalls == 0,
                       "non-GLOBAL postponed compilation retains its original early return and source lifetime");
        audit::Destroy(deferred); glsl_sema_mode = VGL_MODE_GLOBAL;
        PrepareShaderPack("/nonexistent-later-root");
        audit::Require(g_shaderPack.Entries() == 2 && startupSummaries == 1 && lookupSummaries == 1,
                       "repeated prepare cannot replace storage or repeat the once-only attestations");
        failCopy = true; LoadOne(false);
        audit::Require(!failCopy && copyFailures == 1, "copy-allocation failure falls through with original source still intact");
        failCandidateSize = 176; LoadOne(false);
        failCandidateSize = sizeof(matrix_uniform); LoadOne(false);
        failCheck = true; LoadOne(false);
        failRegistration = true; LoadOne(false);
        audit::Require(rejectedCandidates == 4, "all candidate allocation/check/registration failures actually reached fallback");
        // Pack is an immutable build artifact: loose-file damage cannot alter its exact bytes.
        audit::fs::remove(audit::CachePath(audit::root, 'v', 0x78af5f94892f3950ull));
        LoadOne(true);
        // A different source was not packed; exercise the unchanged compiler/writer/next-read.
        CompileMiss();
        audit::root = folder + "/other-v3"; audit::SetRoot(audit::root);
        StageFile(audit::CachePath(audit::root, 'v', 0x78af5f94892f3950ull)); LoadOne(false);
        tinyLooseReads = true; LoadOne(false); tinyLooseReads = false;
        failLooseRead = true; LooseFailure();
        StageFile(audit::CachePath(audit::root, 'v', 0x78af5f94892f3950ull));
        eofLooseRead = true; LooseFailure();
        StageFile(audit::CachePath(audit::root, 'v', 0x78af5f94892f3950ull));
        failLooseClose = true; LooseFailure();
        audit::Require(!failLooseRead && !eofLooseRead && !failLooseClose, "every original-reader I/O fault actually ran");
    }
    std::printf("PASS: real driver shader-pack %s trace=%d, exact source/binary/matrix state, hit/fallback/OOM/root/partial coverage (SDK/GXM doubles, not FPS)\n",
                argv[1], VGX_COLDTRACE);
}
