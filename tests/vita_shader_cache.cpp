// Real host + SDK open wrapper + unmodified frozen driver cache/serialization code.
// Compiler, reflection and GXM accept a synthetic binary; this is not native shader validation.
#ifndef CACHE_HOST_SOURCE
#define CACHE_HOST_SOURCE "../platform/vgx/vita/vgx_host_galaxy_vita.cpp"
#endif
#include CACHE_HOST_SOURCE
#include "vita_shader_cache_fixture.h"
#include "vita_shader_cache_audit.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <new>
#include <set>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

namespace audit {
namespace fs = std::filesystem;
std::atomic<unsigned> mkdirCalls{0}, openCalls{0}, readCalls{0}, writeCalls{0}, seekCalls{0}, closeCalls{0}, clockCalls{0};
std::atomic<unsigned> compilerCalls{0}, nativeSources{0}, hostAllocations{0}, sdkErrors{0};
std::atomic<uint64_t> nowUs{1000};
std::atomic<int> mkdirFailure{0};
std::atomic<int> openFailure{0}, openErrno{0};
std::mutex sdkMutex;
std::string lastOpenPath, lastMkdirPath;
int lastFlags = 0, lastMode = 0, openIncomingErrno = 0;
thread_local bool countHostAllocation = false;
std::array<uint8_t, 24> binary{};
std::string root;
unsigned nextShader = 1;
GLuint expectedShader = 0;
GLsizei expectedCount = 0;
const GLchar* const* expectedStrings = nullptr;
const GLint* expectedLengths = nullptr;

void Require(bool good, const char* why) {
    if (!good) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
uint64_t Clock() noexcept { ++clockCalls; return nowUs.load(); }
uint32_t Thread() noexcept { return 1; }
int SdkError(int error) { return static_cast<int>(0x80010000u | static_cast<unsigned>(error)); }
std::vector<uint8_t> Bytes(const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void Put(const std::string& file, const std::vector<uint8_t>& bytes) {
    fs::create_directories(fs::path(file).parent_path());
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(out.good(), "prepare a real cache file");
}
void SetRoot(const std::string& value) {
    Require(value.size() < sizeof(vgl_shader_cache_path), "bounded test cache root");
    std::strcpy(vgl_shader_cache_path, value.c_str());
    if (!value.empty()) { fs::create_directories(value + "/v"); fs::create_directories(value + "/f"); }
}
void ResetCounts() {
    mkdirCalls = openCalls = readCalls = writeCalls = seekCalls = closeCalls = clockCalls = 0;
    compilerCalls = nativeSources = hostAllocations = sdkErrors = 0;
}
std::string CachePath(const std::string& base, char kind, uint64_t hash) {
    char tail[64];
    std::snprintf(tail, sizeof(tail), "/%c/%02X/%llX.gxp", kind, static_cast<unsigned>(hash >> 56), static_cast<unsigned long long>(hash));
    return base + tail;
}
GLuint Create(GLenum type) {
    countHostAllocation = true;
    const GLuint id = vgxvita_glCreateShader(type);
    countHostAllocation = false;
    return id;
}
void Source(GLuint id, GLsizei count, const char* const* parts, const GLint* lengths) {
    expectedShader = id; expectedCount = count; expectedStrings = parts; expectedLengths = lengths;
    countHostAllocation = true;
    vgxvita_glShaderSource(id, count, parts, lengths);
    countHostAllocation = false;
}
void Destroy(GLuint id) {
    shader* s = shaders[id - 1];
    std::free(s->source);
    std::free(const_cast<SceGxmProgram*>(s->prog));
    std::free(s->log);
    Require(s->mat == nullptr && s->unif_blk == nullptr, "fixture has no reflection allocations");
    std::free(s);
    shaders[id - 1] = nullptr;
    nextShader = 1;
}
void SeedShader(const std::string& file) {
    std::vector<uint8_t> data(28, 0);
    for (unsigned i = 4; i != data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7);
    Put(file, data);
}
void Hit(const std::string& mode) {
    const bool fragment = mode == "hit-fragment";
    const char* parts[] = {"abc"};
    SeedShader(CachePath(root, fragment ? 'f' : 'v', 0x78af5f94892f3950ull));
    ResetCounts();
    const auto id = Create(fragment ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
#if VGX_COLDTRACE
    auto& recorder = vgx::coldtrace::g_recorder;
    Require(recorder.Arm(Clock, Thread, 1), "arm actual source hash annotations");
    {
    vgx::coldtrace::Scope item(recorder, vgx::coldtrace::Key{1, 1, 0, 0, 0, 0});
#endif
    Source(id, 1, parts, nullptr);
    Require(mkdirCalls == 0, "cache-hit shader source must not call mkdir");
    glCompileShader(id);
#if VGX_COLDTRACE
    }
    recorder.RequestFreeze();
    const auto view = recorder.Frozen();
    Require(view.data && view.data->worstCount == 1 && view.data->worst[0].shaderCount == 1,
            "actual host preserves one probe source identity");
    const auto& identity = view.data->worst[0].shaders[0];
    Require(identity.sourceHash == 0x78af5f94892f3950ull && identity.kind == (fragment ? 2u : 1u), "probe hash and kind unchanged");
#endif
    Require(mkdirCalls == 0 && openCalls == 1 && seekCalls == 2 && readCalls == 1 && closeCalls == 1 && writeCalls == 0 && compilerCalls == 0,
            "warm GXP hit adds no directory or extra file operations to actual driver lookup");
    Require(nativeSources == 1, "source forwarded to driver exactly once");
#if !VGX_COLDTRACE
    Require(hostAllocations == 0 && clockCalls == 0, "normal source/cache path adds no host allocations or diagnostic clocks");
#endif
    Destroy(id);
}
void Miss(const std::string& mode) {
    const bool multi = mode == "miss-multi", nul = mode == "miss-nul", large = mode == "miss-large";
    const std::string longPart(65536, 'q');
    const char embedded[] = {'a', 0, 'b', 'c'};
    const char* parts[] = {large ? longPart.c_str() : multi ? "a" : nul ? embedded : "abcTAIL", "bcTAIL"};
    GLint lengths[] = {large ? 65536 : multi ? -1 : nul ? 4 : 3, 2};
    const uint64_t hash = large ? XXH3_64bits(longPart.data(), longPart.size()) : nul ? XXH3_64bits(embedded, 4) : 0x78af5f94892f3950ull;
    const auto file = CachePath(root, 'v', hash);
    ResetCounts();
    const auto id = Create(GL_VERTEX_SHADER);
#if VGX_COLDTRACE
    auto& recorder = vgx::coldtrace::g_recorder;
    Require(recorder.Arm(Clock, Thread, 1), "arm actual miss SDK accounting");
    {
    vgx::coldtrace::Scope item(recorder, vgx::coldtrace::Key{1, 1, 0, 0, 0, 0});
#endif
    Source(id, multi ? 2 : 1, parts, lengths);
    Require(mkdirCalls == 0, "cache miss is not known until actual native compile lookup");
    glCompileShader(id);
#if VGX_COLDTRACE
    }
    recorder.RequestFreeze();
    const auto view = recorder.Frozen();
    Require(view.data && view.data->worstCount == 1, "actual miss produces one complete item");
    const auto& item = view.data->worst[0];
    const auto mkdir = static_cast<size_t>(vgx::coldtrace::Phase::Mkdir), open = static_cast<size_t>(vgx::coldtrace::Phase::Open);
    Require(item.calls[mkdir] == 1 && item.inclusiveUs[mkdir] == 10 && item.exclusiveUs[mkdir] == 10 &&
            item.calls[open] == 2 && item.inclusiveUs[open] == 20 && item.exclusiveUs[open] == 20,
            "actual cache-write preparation is separate from Open inclusive and exclusive timing");
    uint64_t partition = item.selfUs;
    for (auto us : item.exclusiveUs) partition += us;
    Require(item.totalUs == 50 && partition == item.totalUs, "actual miss SDK operations partition exact scripted wall time");
    Require(item.shaderCount == 1 && item.shaders[0].sourceHash == hash, "explicit, joined and binary probe source hashes match native cache keys");
#endif
    Require(mkdirCalls == 1 && compilerCalls == 1 && openCalls == 2 && writeCalls == 1 && closeCalls == 1 && readCalls == 0,
            "real miss makes one bucket and persists through unchanged frozen cache writer");
    Require(lastOpenPath == file && lastFlags == 0x602 && lastMode == 0777, "actual driver hash, path, flags and mode are preserved");
    const auto persisted = Bytes(file);
    Require(persisted.size() == 28 && std::all_of(persisted.begin(), persisted.begin() + 4, [](uint8_t b) { return b == 0; }) &&
            std::equal(binary.begin(), binary.end(), persisted.begin() + 4), "actual frozen serializer writes exact binary bytes");
    Require(openIncomingErrno == EAGAIN, "mkdir preparation preserves compiler's errno before real write open");
#if !VGX_COLDTRACE
    Require(hostAllocations == 0 && clockCalls == 0, "normal explicit/multipart source forwarding allocates and times nothing");
#endif
    Destroy(id);
    const auto again = Create(GL_VERTEX_SHADER);
    ResetCounts();
    Source(again, multi ? 2 : 1, parts, lengths);
    glCompileShader(again);
    Require(compilerCalls == 0 && mkdirCalls == 0 && openCalls == 1 && readCalls == 1 && Bytes(file) == persisted,
            "next driver encounter reads the identical persisted binary without compiling or mkdir");
    Require(shaders[again - 1]->size == 24 && std::memcmp(shaders[again - 1]->prog, binary.data(), 24) == 0,
            "actual frozen unserializer restores the exact binary");
    Destroy(again);
}
void WarmBuckets() {
    std::array<std::string, 256> source;
    unsigned remaining = 256;
    for (unsigned i = 0; remaining && i != 10000; ++i) {
        std::string text = "cached-fixture-" + std::to_string(i);
        const auto bucket = static_cast<unsigned>(XXH3_64bits(text.data(), text.size()) >> 56);
        if (source[bucket].empty()) { source[bucket] = std::move(text); --remaining; }
    }
    Require(remaining == 0, "independently prepare a source for every hash top byte");
    for (const auto& text : source) for (char kind : {'v', 'f'}) SeedShader(CachePath(root, kind, XXH3_64bits(text.data(), text.size())));
    ResetCounts();
    for (const auto& text : source) for (const auto kind : {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER}) {
        const auto id = Create(kind); const char* part = text.c_str();
        Source(id, 1, &part, nullptr); glCompileShader(id); Destroy(id);
    }
    Require(mkdirCalls == 0 && compilerCalls == 0 && openCalls == 512 && seekCalls == 1024 && readCalls == 512 && closeCalls == 512,
            "all 512 cached kind/buckets add no metadata or redundant file operations");
#if !VGX_COLDTRACE
    Require(hostAllocations == 0 && clockCalls == 0, "512 normal cached shaders need no host allocation or clock calls");
#endif
}
void WriteFailure() {
    openFailure = 1;
    const char* part = "abc";
    const auto file = CachePath(root, 'v', 0x78af5f94892f3950ull);
    for (unsigned pass = 0; pass != 2; ++pass) {
        const auto id = Create(GL_VERTEX_SHADER); Source(id, 1, &part, nullptr); glCompileShader(id); Destroy(id);
        if (pass == 0) Require(!fs::exists(file) && sdkErrors == 2 && compilerCalls == 1, "failed cache write/close preserve real SDK errors without losing the compiled shader");
        else Require(fs::exists(file) && compilerCalls == 2 && mkdirCalls == 1, "later miss can retry persistence without another successful mkdir");
    }
}
void OpenPaths(const std::string& mode) {
    const auto valid = CachePath(root, 'v', 0x78af5f94892f3950ull);
    if (mode == "grammar") {
        const std::array<std::string, 12> invalid = {root, root + "/v/78/.gxp", root + "/v/78/078AF5F94892F3950.gxp",
            root + "/v/78/78af5f94892f3950.gxp", root + "/v/79/78AF5F94892F3950.gxp", valid + "TAIL",
            root + "/v/78/78AF5F94892F3950.GXP", root + "/x/78/78AF5F94892F3950.gxp",
            root + "/v/7/78AF5F94892F3950.gxp", root + "/v//78/78AF5F94892F3950.gxp", root + "x/v/78/78AF5F94892F3950.gxp", "x"};
        for (const auto& name : invalid) { const int fd = sceIoOpen(name.c_str(), 0x602, 0777); if (fd >= 0) sceIoClose(fd); }
        Require(mkdirCalls == 0, "malformed, wrong-root and noncanonical cache names do no preparation");
        for (const auto [flags, modeBits] : {std::pair<int, int>{0x602, 0666}, {SCE_O_RDONLY, 0777}, {0x602 | SCE_O_APPEND, 0777}, {SCE_O_RDWR | SCE_O_CREAT | SCE_O_TRUNC, 0777}}) {
            const int fd = sceIoOpen(valid.c_str(), flags, modeBits); if (fd >= 0) sceIoClose(fd);
        }
        Require(mkdirCalls == 0, "ordinary SDK reads, seed modes and other write flags remain untouched");
        return;
    }
    if (mode == "all-buckets") {
        for (char kind : {'v', 'f'}) for (unsigned i = 0; i != 256; ++i) {
            const uint64_t hash = static_cast<uint64_t>(i) << 56 | 1;
            const auto file = CachePath(root, kind, hash);
            const auto fd = sceIoOpen(file.c_str(), 0x602, 0777);
            Require(fd >= 0, "every native kind/bucket creates the needed directory"); sceIoClose(fd);
            const auto repeat = sceIoOpen(file.c_str(), 0x602, 0777); Require(repeat >= 0, "repeat write succeeds"); sceIoClose(repeat);
        }
        Require(mkdirCalls == 512 && openCalls == 1024, "one metadata call per actual bucket, no redundant opens");
        return;
    }
    if (mode == "root-change") {
        for (const auto& next : {root, root + "-other", root}) {
            SetRoot(next); const auto fd = sceIoOpen(CachePath(next, 'v', 0x78af5f94892f3950ull).c_str(), 0x602, 0777);
            Require(fd >= 0, "new cache root must prepare independently"); sceIoClose(fd);
        }
        Require(mkdirCalls == 3, "bucket memoization resets on cache-root changes"); return;
    }
    if (mode == "concurrent") {
        std::array<std::thread, 4> workers;
        for (unsigned t = 0; t != workers.size(); ++t) workers[t] = std::thread([t] {
            for (unsigned i = 1; i != 33; ++i) {
                const auto file = CachePath(root, 'v', 0x7800000000000000ull | (t * 32 + i));
                const auto fd = sceIoOpen(file.c_str(), 0x602, 0777); Require(fd >= 0, "concurrent private cache open succeeds"); sceIoClose(fd);
            }
        });
        for (auto& thread : workers) thread.join();
        Require(mkdirCalls == 1 && openCalls == 128 && closeCalls == 128, "concurrent matched writes safely share one successful bucket preparation"); return;
    }
    if (mode == "concurrent-buckets") {
        std::atomic<bool> done{false};
        std::thread reader([&] {
            while (!done.load()) { unsigned made, ms; vgxvita_cacheFolderStats(&made, &ms); Require(made <= 128 && ms == 0, "concurrent statistics remain coherent"); }
        });
        std::array<std::thread, 4> workers;
        for (unsigned t = 0; t != workers.size(); ++t) workers[t] = std::thread([t] {
            for (unsigned i = 0; i != 32; ++i) {
                const auto fd = sceIoOpen(CachePath(root, 'v', static_cast<uint64_t>(t * 32 + i) << 56 | 1).c_str(), 0x602, 0777);
                Require(fd >= 0, "concurrent distinct-bucket write succeeds"); sceIoClose(fd);
            }
        });
        for (auto& thread : workers) thread.join();
        done = true; reader.join();
        unsigned made, ms; vgxvita_cacheFolderStats(&made, &ms);
        Require(mkdirCalls == 128 && made == 128 && openCalls == 128 && closeCalls == 128, "concurrent distinct buckets and statistics share safe memoization"); return;
    }
    if (mode == "max-root" || mode == "trailing-root") {
        const auto next = mode == "max-root" ? root + "/" + std::string(255 - root.size() - 1, 'r') : root + "/";
        SetRoot(next); const auto file = CachePath(next, 'f', 0xff00000000000001ull);
        const auto fd = sceIoOpen(file.c_str(), 0x602, 0777); Require(fd >= 0 && mkdirCalls == 1, "helper bounds hold at root declaration limit and for exact trailing-slash spelling"); sceIoClose(fd); return;
    }
    if (mode == "errno") openErrno = EDOM;
    if (mode == "retry") mkdirFailure = 1;
    if (mode == "missing-parent") { fs::remove_all(root + "/v"); fs::remove_all(root + "/f"); }
    if (mode == "exists") fs::create_directory(root + "/v/78");
    if (mode == "blocked") { std::ofstream collision(root + "/v/78"); collision << "not a directory"; }
    if (mode == "empty") SetRoot("");
    if (mode == "unterminated") std::memset(vgl_shader_cache_path, 'X', 256);
    for (unsigned attempt = 0; attempt != 2; ++attempt) {
        errno = E2BIG;
        const auto fd = sceIoOpen(valid.c_str(), 0x602, 0777);
        if (mode == "errno") Require(errno == EDOM, "real open's outgoing errno survives the wrapper");
        Require(openIncomingErrno == E2BIG, "real open receives original errno even when mkdir fails");
        if (fd >= 0) sceIoClose(fd);
        if (mode == "retry") Require((attempt == 0) == (fd < 0), "a transient mkdir failure is retried on the next write");
        if (mode == "blocked") Require(fd < 0, "unwritable bucket preserves the real SDK open error");
        if (mode == "missing-parent") Require(fd < 0, "missing parent preserves real SDK open failure without inventing cache structure");
    }
    Require(mkdirCalls == ((mode == "empty" || mode == "unterminated") ? 0u : (mode == "retry" || mode == "missing-parent") ? 2u : 1u),
            "only successful or EEXIST directory preparation is memoized");
}
#if VGX_COLDTRACE
void RetryPhases() {
    using namespace vgx::coldtrace;
    Require(g_recorder.Arm(Clock, Thread, 1), "arm actual mkdir failure accounting");
    { Scope item(g_recorder, Key{}); OpenPaths("retry"); }
    g_recorder.RequestFreeze(); const auto view = g_recorder.Frozen();
    Require(view.data && view.data->worstCount == 1, "failed mkdir item is complete");
    const auto& item = view.data->worst[0]; const auto mkdir = static_cast<size_t>(Phase::Mkdir), open = static_cast<size_t>(Phase::Open);
    Require(item.calls[mkdir] == 2 && item.failures[mkdir] == 1 && item.firstError[mkdir] == SdkError(EIO) &&
            item.inclusiveUs[mkdir] == 20 && item.exclusiveUs[mkdir] == 20 && item.calls[open] == 2 && item.failures[open] == 1 &&
            item.inclusiveUs[open] == 20 && item.exclusiveUs[open] == 20 && item.totalUs == 50,
            "actual failed mkdir and retry are separate from Open phases and preserve SDK error");
}
#endif
}

void* operator new(std::size_t n) {
    if (audit::countHostAllocation) ++audit::hostAllocations;
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
#ifdef CACHE_WEAK_ROOT
    audit::Require(argc == 2 && !std::strcmp(argv[1], "weak-root"), "weak-symbol fixture mode");
    audit::Require(&vgl_shader_cache_path[0] == nullptr, "fixture leaves weak cache symbol unresolved");
    const auto fd = sceIoOpen("/results/no-cache-root/v/78/78AF5F94892F3950.gxp", 0x602, 0777);
    if (fd >= 0) sceIoClose(fd);
    audit::Require(audit::mkdirCalls == 0 && audit::openCalls == 1, "missing native shader-cache support is a transparent pass-through");
    std::puts("PASS: actual shader-cache weak-root pass-through"); return 0;
#else
    audit::Require(argc == 2, "supply shader-cache scenario");
    const std::string mode = argv[1];
    audit::root = "/results/cache-" + mode + "-" + std::to_string(getpid());
    audit::SetRoot(audit::root);
    audit::ResetCounts();
    if (mode.rfind("hit", 0) == 0) audit::Hit(mode);
    else if (mode.rfind("miss", 0) == 0) audit::Miss(mode);
    else if (mode == "warm-buckets") audit::WarmBuckets();
    else if (mode == "write-failure") audit::WriteFailure();
#if VGX_COLDTRACE
    else if (mode == "retry") audit::RetryPhases();
#endif
    else audit::OpenPaths(mode);
    audit::Require(audit::mkdirFailure == 0, "all injected mkdir failures consumed");
    std::printf("PASS: actual shader-cache %s (trace=%d; synthetic compiler/GXM, not native timing)\n", argv[1], VGX_COLDTRACE);
    audit::fs::remove_all(audit::root); audit::fs::remove_all(audit::root + "-other");
#endif
    return 0;
}
