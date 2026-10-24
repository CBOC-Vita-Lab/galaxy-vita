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
    for (unsigned i = 164; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>(i * 7);
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
    const auto id = audit::Create(GL_VERTEX_SHADER);
    const char* text = "abc"; audit::Source(id, 1, &text, nullptr);
    glCompileShader(id);
    audit::Require(audit::compilerCalls == 0 && audit::nativeSources == 1 && audit::mkdirCalls == 0 && audit::writeCalls == 0,
                   "cached load preserves source forwarding and never compiles or writes");
    audit::Require(audit::openCalls == (ram ? 0u : 1u) && audit::seekCalls == (ram ? 0u : 2u) &&
                   audit::readCalls == (ram ? 0u : 1u) && audit::closeCalls == (ram ? 0u : 1u),
                   "RAM hit removes five file calls; ordinary fallback preserves every original call");
    CheckShader(id);
}
}
extern "C" {
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
    return __real_vglMalloc(size);
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
    audit::root = folder + "/v2";
    if (mode == "prepare") {
        audit::fs::remove_all(argv[2]); audit::SetRoot(audit::root);
        for (char stage : {'v', 'f'}) StageFile(audit::CachePath(audit::root, stage, 0x78af5f94892f3950ull), stage);
        char identity[512]; ShaderPackIdentity(identity, sizeof identity);
        std::puts(identity); return 0;
    }
    audit::SetRoot(audit::root);
    LoadOne(false); // Independent baseline: new driver, pack not loaded yet.
    if (mode == "oom") failHostPack = true;
    PrepareShaderPack(folder.c_str());
    if (mode == "oom") {
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
        audit::Require(g_shaderPack.Entries() == 2, "repeated prepare cannot replace storage after startup publication");
        failCopy = true; LoadOne(false);
        audit::Require(!failCopy && copyFailures == 1, "copy-allocation failure falls through with original source still intact");
        // Pack is an immutable build artifact: loose-file damage cannot alter its exact bytes.
        audit::fs::remove(audit::CachePath(audit::root, 'v', 0x78af5f94892f3950ull));
        LoadOne(true);
        // A different source was not packed; exercise the unchanged compiler/writer/next-read.
        audit::Miss("miss-large");
        audit::root = folder + "/other-v2"; audit::SetRoot(audit::root);
        StageFile(audit::CachePath(audit::root, 'v', 0x78af5f94892f3950ull)); LoadOne(false);
    }
    std::printf("PASS: real driver shader-pack %s trace=%d, exact source/binary/matrix state, hit/fallback/OOM/root/partial coverage (SDK/GXM doubles, not FPS)\n",
                argv[1], VGX_COLDTRACE);
}
