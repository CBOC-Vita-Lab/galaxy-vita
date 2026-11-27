// LOD invalidation through the real BP writer, cached state-list replay and shader/uniform builders.
#include "../platform/vgx/src/vgx_draw.cpp"
#include "../platform/vgx/src/vgx_stats.cpp"
#include "vgx_hle.h"
#include <chrono>
#include <cstdarg>

extern "C" int64_t port_host_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
extern "C" int64_t port_thread_cpu_us() { return 0; }
extern "C" void port_log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}
namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}

namespace {
using namespace vgx;
unsigned failures = 0, cases = 0;
uint64_t trace = 14695981039346656037ull;
alignas(4096) uint8_t lists[4096] = {};
void Check(bool ok, const char* name) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
}
void Mix(const void* data, size_t bytes) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) trace = (trace ^ p[i]) * 1099511628211ull;
}
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
void ClearDirty() {
    g.bpDirty = g.bpKeyDirty = g.texDirty = false;
    g.texSlotDirty = g.bpTexStale = 0;
}
struct Case {
    uint32_t mode0, mode1, offset, value;
    float bias;
    bool key, tev;
};
constexpr uint32_t mip = 6u << 5, half = 16u << 9, whole = 32u << 9, max6 = 96u << 8;
constexpr Case kCases[] = {
    {mip | half, max6, 0, mip | half | 1, 0.5f, false, false},        // wrap
    {mip | half, max6, 0, mip | half | 16, 0.5f, false, false},       // magnification
    {mip | half, max6, 0, (5u << 5) | half, 0.5f, false, false},     // another mip filter
    {mip | half, max6, 4, max6 | 32, 0.5f, false, false},            // minimum LOD
    {mip | half, max6, 4, 80u << 8, 0.5f, false, false},             // maximum LOD still >= 1
    {mip | half, max6, 0, mip | whole, 1.0f, false, true},           // nonzero bias value only
    {mip | half, max6, 0, mip | (240u << 9), -0.5f, false, true},    // signed bias value only
    {mip | half, max6, 0, mip, 0.0f, true, true},                    // bias removed
    {mip, max6, 0, mip | half, 0.5f, true, true},                    // bias introduced
    {mip | half, max6, 4, 15u << 8, 0.0f, true, true},              // maximum LOD below 1
    {mip | half, 15u << 8, 4, 16u << 8, 0.5f, true, true},          // maximum LOD reaches 1
    {(4u << 5) | half, max6, 0, (4u << 5) | whole, 0.0f, false, false}, // no mip filter
    {mip | half, 0, 0, mip | whole, 0.0f, false, false},             // no mip levels
};
void Output(unsigned map, float bias, bool enabled) {
    BP(0x00, 1);
    BP(0x28, (1u << 6) | map | (7u << 7));
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19));
    FsKey fs;
    BuildFsKey(fs);
    const bool biased = enabled && bias != 0;
    Check(fs.stage[0].lodBias == biased, "actual fragment key follows effective bias presence");
    UniArena uni;
    uint32_t offsets[UB_COUNT], lengths[UB_COUNT] = {};
    for (auto& off : offsets) off = kNoBlock;
    const uint16_t sizes[8] = {64,64,64,64,64,64,64,64};
    split::BlockCtx context{&uni, offsets, lengths, true, false, 1};
    split::BuildTevC(context, g, fs, sizes, sizes, false);
    if (biased) Check(uni[offsets[UB_TEV] + TEV_LODBIAS + map] == bias, "actual TEV block keeps the exact signed bias");
    ShaderSource shader;
    GenerateFragmentShader(fs, shader);
    Mix(&fs, sizeof fs);
    Mix(uni.data(), uni.size() * sizeof(float));
    Mix(shader.text.data(), shader.text.size());
}
void Run(const Case& c, unsigned map, bool replay, bool enabled, bool traceOnly) {
    ResetState();
    Check(g.keyHash == KeyHashOf(g), "ResetState initializes a coherent hash");
    const unsigned mode0 = map < 4 ? 0x80 + map : 0xA0 + map - 4;
    BP(mode0, c.mode0);
    BP(mode0 + 4, c.mode1);
    const unsigned reg = mode0 + c.offset;
    const uint64_t before = g.keyHash;
    uint8_t* list = lists + cases * 16;
    if (replay) {
        list[0] = 0x61;
        const uint32_t word = reg << 24 | c.value;
        for (unsigned i = 0; i < 4; ++i) list[i + 1] = static_cast<uint8_t>(word >> (24 - i * 8));
        hle::NotifyGuestWrite(list, 5);
        Check(StateListCall(list, 5), "state list is parsed by production code");
        BP(reg, c.offset ? c.mode1 : c.mode0);
    }
    ClearDirty();
    if (replay) Check(StateListFastHit(list, 5), "cached replay takes the real inline texture-register path");
    else BP(reg, c.value);
    if (!traceOnly) {
        Check(g.bpKeyDirty == (enabled && c.key), "only effective bias presence changes invalidate shader keys");
        Check(g.bpDirty == (enabled && c.tev), "only an effective bias change invalidates TEV uniforms");
        Check((g.keyHash != before) == (enabled && c.key), "sampler-only and nonzero bias changes keep the shader hash");
    }
    Check(g.keyHash == KeyHashOf(g), "incremental and recomputed shader hashes agree");
    const uint8_t bit = static_cast<uint8_t>(1u << map);
    Check(g.texDirty && g.texSlotDirty == bit && g.bpTexStale == bit, "wrap/filter/min/max LOD writes still invalidate the correct texture binding");
    Check(g.bp[reg] == c.value, "the complete texture register is preserved");
    ClearDirty();
    const uint64_t after = g.keyHash;
    BP(reg, c.value);
    Check(!g.bpKeyDirty && !g.bpDirty && !g.texDirty && g.keyHash == after, "repeated writes remain no-ops");
    BpTexSlotSync(map);
    const uint32_t raw0 = c.offset ? c.mode0 : c.value, raw1 = c.offset ? c.value : c.mode1;
    const TexObj& object = g.tex[map].obj;
    Check(object.wrapS == (raw0 & 3) && object.wrapT == ((raw0 >> 2) & 3) && object.magFilter == ((raw0 >> 4) & 1),
          "the binding consumer receives unchanged sampler choices");
    Check(object.minLod == static_cast<float>(raw1 & 255) / 16 && object.maxLod == static_cast<float>((raw1 >> 8) & 255) / 16,
          "minimum and maximum LOD still reach the texture object");
    const float sampler[] = {float(object.wrapS), float(object.wrapT), float(object.magFilter), float(object.minFilter), object.minLod, object.maxLod, object.lodBias};
    Mix(sampler, sizeof sampler);
    Output(map, c.bias, enabled);
    ++cases;
}
void Interleaved(bool enabled) {
    const float triangle[] = {-1,-1,-0.5f,0,0,0, 1,-1,-0.5f,0,1,0, 0,1,-0.5f,0,0.5f,1};
    for (unsigned map = 0; map < 8; ++map) {
        ResetState();
        BP(0x00, 1);
        BP(0x28, (1u << 6) | map | (7u << 7));
        BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19));
        const unsigned mode0 = map < 4 ? 0x80 + map : 0xA0 + map - 4;
        const auto draw = [&](float bias) {
            DrawDecoded(0x90, VL_TEX0, reinterpret_cast<const uint8_t*>(triangle), 3);
            const bool biased = enabled && bias != 0;
            Check(cur.fs.stage[0].lodBias == biased, "intermediate real draw uses the right shader key");
            float value = 0;
            if (biased) {
                value = g_buf.uni[g_blockOff[UB_TEV] + TEV_LODBIAS + map];
                Check(value == bias, "intermediate real draw refreshes the TEV bias, including same-key changes");
            }
            Check(g.keyHash == KeyHashOf(g), "interleaved draws preserve hash coherence");
            Mix(&cur.fs, sizeof cur.fs);
            Mix(&value, sizeof value);
        };
        BP(mode0, mip | half); draw(0);
        BP(mode0 + 4, max6); draw(0.5f);
        BP(mode0, mip | whole); draw(1.0f);
        BP(mode0 + 4, 0); draw(0);
        BP(mode0, mip | (240u << 9)); draw(0);
        BP(mode0 + 4, max6); draw(-0.5f);
        ResetState();
        Check(g.keyHash == KeyHashOf(g), "reset after biased draws restores a coherent hash");
    }
}
}

int main(int argc, char** argv) {
    const bool enabled = argc > 1 && std::atoi(argv[1]) != 0;
    const bool traceOnly = argc > 2 && std::strcmp(argv[2], "trace") == 0;
    Check(FixLodBias() == enabled, "the requested diagnostic switch is in force");
    hle::RegisterGuestRam(lists, sizeof lists);
    for (bool replay : {false, true})
        for (unsigned map = 0; map < 8; ++map)
            for (const Case& c : kCases) Run(c, map, replay, enabled, traceOnly);
    Interleaved(enabled);
    std::printf("LOD state: %u cases, %u failures; semantic_trace=%016llx\n", cases, failures, static_cast<unsigned long long>(trace));
    return failures ? 1 : 0;
}
