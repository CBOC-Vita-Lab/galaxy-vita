// Exercise the real planner and decoders, including untouched general-loop oracle.
#include "../platform/vgx/src/vgx_state.cpp"
#include <chrono>
#include <string>

extern "C" int64_t port_host_time_ns() { return 0; }
extern "C" int64_t port_thread_cpu_us() { return 0; }
extern "C" void port_log(const char*, ...) {}

namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}

namespace {
unsigned failures = 0, cases = 0;
uint64_t vertices = 0, eligibleVertices = 0;
static_assert(offsetof(vgx::Plan, n) == 4, "direct defaults consume padding, not new plan storage");
void Require(bool ok, const char* message) {
    if (!ok) {
        if (failures < 12) std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
void Case(unsigned type, unsigned components, unsigned frac, unsigned extras, unsigned mode,
          unsigned count, bool folded, bool enabled) {
    using namespace vgx;
    PlanCache cache{};
    uint8_t vcd[26] = {};
    VtxAttrFmt vat[8][26] = {};
    vcd[A_POS] = static_cast<uint8_t>(mode);
    vcd[A_PNMTXIDX] = (extras & 1) ? T_DIRECT : T_NONE;
    vcd[A_TEX0MTXIDX] = (extras & 2) ? T_DIRECT : T_NONE;
    vcd[A_CLR0] = (extras & 4) ? T_DIRECT : T_NONE;
    vcd[A_TEX0] = (extras & 8) ? T_DIRECT : T_NONE;
    vat[0][A_POS] = {static_cast<uint8_t>(components == 3), static_cast<uint8_t>(type), static_cast<uint8_t>(frac)};
    vat[0][A_CLR0] = {1, 5, 0};
    vat[0][A_TEX0] = {1, 4, 0};
    bool dirty = true;
    const Plan& base = GetPlanIn(cache, dirty, vcd, vat, 0, false);
    const Plan& p = folded ? GetFoldedPlanIn(cache, 0, base) : base;
    const bool eligible = p.direct && !(extras & 3);
    Require(p.directDefaults == (eligible && enabled ? 0 : p.defaults), "only eligible plans remove the default traversal");
    std::vector<uint8_t> src(static_cast<size_t>(count) * p.streamSize + 32);
    uint32_t random = 0x5A17C93Du ^ (type << 9) ^ frac;
    for (uint8_t& b : src) { random = random * 1664525u + 1013904223u; b = static_cast<uint8_t>(random >> 24); }
    if (type == C_F32 && mode == T_DIRECT) {
        const uint32_t edge[] = {0, 0x80000000u, 1, 0x7F800000u, 0xFF800000u, 0x7FC12345u, 0x7F812345u, 0x7F7FFFFFu};
        for (int i = 0; i < p.n; ++i) if (p.a[i].attr == A_POS)
            for (unsigned v = 0; v < count; ++v) for (unsigned c = 0; c < components; ++c) {
                const uint32_t word = edge[(v + c) % 8];
                uint8_t* dst = src.data() + v * p.streamSize + p.a[i].srcOff + c * 4;
                for (unsigned b = 0; b < 4; ++b) dst[b] = static_cast<uint8_t>(word >> (24 - b * 8));
            }
    }
    std::vector<uint8_t> array(4096);
    for (uint8_t& b : array) { random = random * 1664525u + 1013904223u; b = static_cast<uint8_t>(random >> 24); }
    g.arrays[A_POS] = {array.data(), static_cast<uint32_t>(array.size()), 16, (extras & 4) != 0};
    const size_t bytes = static_cast<size_t>(count) * p.layout.stride;
    std::vector<uint8_t> actual(bytes + 32, 0xA5), expected(bytes + 32, 0xA5);
    ArraySpans a{}, e{};
    DecodeVertices(p, src.data(), count, actual.data() + 16, &a);
    const uint32_t actualMax = LastDecodedPosMtxMax();
    DecodeVerticesGeneral(p, src.data(), count, expected.data() + 16, &e, false);
    const uint32_t expectedMax = LastDecodedPosMtxMax();
    Require(actual == expected, "complete vertices and both guards match general production decoder");
    Require(actualMax == expectedMax, "highest matrix row is unchanged");
    Require(a.mask == e.mask && std::memcmp(a.lo, e.lo, sizeof a.lo) == 0 && std::memcmp(a.hi, e.hi, sizeof a.hi) == 0,
            "array spans remain unchanged");
    if (eligible && enabled && count) {
        const AttrPlan* pos = nullptr;
        for (int i = 0; i < p.n; ++i) if (p.a[i].attr == A_POS) pos = &p.a[i];
        std::vector<uint8_t> kernel(bytes + 32, 0xA5);
        pos->fn(src.data() + pos->srcOff, kernel.data() + 16, count, p.streamSize, p.layout.stride, nullptr, 0, 0, pos->scale);
        bool zero = true;
        for (unsigned v = 0; v < count; ++v) {
            const uint8_t* dst = kernel.data() + 16 + v * p.layout.stride;
            for (unsigned i = components * 4; i < 16; ++i) zero &= dst[i] == 0;
        }
        Require(zero, "eligible position kernel itself supplies z/w defaults without a second vertex traversal");
        eligibleVertices += count;
    }
    ++cases;
    vertices += count;
}

void Benchmark() {
    using namespace vgx;
    volatile uint64_t checksum = 0;
    Require(PositionDefaultsMode() == 1, "benchmark requires mode 1, not the oracle");
    for (unsigned type : {0u, 1u, 3u, 4u}) for (unsigned components : {2u, 3u}) for (unsigned count : {4u, 64u, 256u}) {
        PlanCache cache{};
        uint8_t vcd[26] = {};
        VtxAttrFmt vat[8][26] = {};
        vcd[A_POS] = T_DIRECT; vcd[A_TEX0] = T_DIRECT;
        vat[0][A_POS] = {static_cast<uint8_t>(components == 3), static_cast<uint8_t>(type), 0};
        vat[0][A_TEX0] = {1, 4, 0};
        bool dirty = true;
        const Plan& fast = GetPlanIn(cache, dirty, vcd, vat, 0, false);
        Plan old = fast;
        old.directDefaults = old.defaults;
        for (int i = 0; i < old.n; ++i)
            if (old.a[i].attr == A_POS) old.a[i].fn = kAttrFn[0][0][type][components - 1];
        std::vector<uint8_t> src(count * fast.streamSize, 0x19), dst(count * fast.layout.stride);
        const unsigned iterations = 800000 / count;
        auto time = [&](const Plan& plan) {
            const auto start = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < iterations; ++i) DecodeVerticesDirect(plan, src.data(), count, dst.data());
            checksum += dst[0];
            return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        };
        std::vector<double> legacy, fused;
        time(old); time(fast);
        for (unsigned sample = 0; sample < 7; ++sample) {
            if (sample & 1) { fused.push_back(time(fast)); legacy.push_back(time(old)); }
            else { legacy.push_back(time(old)); fused.push_back(time(fast)); }
        }
        std::sort(legacy.begin(), legacy.end()); std::sort(fused.begin(), fused.end());
        std::printf("microbench: type=%u components=%u count=%u draws=%u legacy_us=%.1f fused_us=%.1f ratio=%.3f removed_iterations=%u\n",
                    type, components, count, iterations, legacy[3], fused[3], fused[3] / legacy[3], iterations * count);
    }
    std::printf("microbench checksum=%llu; host-only, not Vita FPS\n", static_cast<unsigned long long>(checksum));
}

void MissingPosition() {
    using namespace vgx;
    PlanCache cache{};
    uint8_t vcd[26] = {};
    VtxAttrFmt vat[8][26] = {};
    bool dirty = true;
    const Plan& p = GetPlanIn(cache, dirty, vcd, vat, 0, false);
    Require(p.directDefaults == 3, "missing position retains both original defaults");
    uint8_t src[1] = {}, actual[16], expected[16];
    std::memset(actual, 0xA5, sizeof actual); std::memset(expected, 0xA5, sizeof expected);
    DecodeVertices(p, src, 1, actual, nullptr);
    DecodeVerticesGeneral(p, src, 1, expected, nullptr, false);
    Require(std::memcmp(actual, expected, sizeof actual) == 0, "missing-position bytes match unchanged decoder");
}
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "bench") { Benchmark(); return failures ? 1 : 0; }
    const bool enabled = argc > 1 && (std::string(argv[1]) == "1" || std::string(argv[1]) == "2");
    const unsigned counts[] = {0, 1, 4, 129, 257};
    for (unsigned type = 0; type < 5; ++type)
        for (unsigned components : {2u, 3u})
            for (unsigned frac = 0; frac < 32; ++frac)
                for (unsigned extras : {0u, 1u, 2u, 3u, 4u, 8u, 12u})
                    for (unsigned count : counts)
                        for (bool folded : {false, true})
                            Case(type, components, frac, extras, 1, count, folded, enabled);
    for (unsigned mode : {2u, 3u})
        for (unsigned type = 0; type < 5; ++type)
            for (unsigned extras : {0u, 1u, 2u, 4u, 12u})
                Case(type, 3, 7, extras, mode, 129, false, enabled);
    Require(vgx::g_posDefaultBad == 0, "runtime exact oracle reports no mismatches");
    if (vgx::PositionDefaultsMode() == 2) Require(vgx::g_posDefaultChecked >= eligibleVertices, "mode 2 really executes the reference path");
    setenv("GXV_POSDEFAULT", enabled ? "0" : "1", 1);
    Case(4, 3, 0, 0, 1, 4, false, enabled);
    MissingPosition();
    std::printf("position-defaults: cases=%u vertices=%llu eligible-kernel-vertices=%llu failures=%u\n", cases,
                static_cast<unsigned long long>(vertices), static_cast<unsigned long long>(eligibleVertices), failures);
    return failures ? 1 : 0;
}
