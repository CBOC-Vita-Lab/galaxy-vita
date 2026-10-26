// Host-only ARM fixture: no game, assets, renderer, or Vita SDK dependency.
#include "vgx_state.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" int64_t port_host_time_ns() { return 0; }
extern "C" int64_t port_thread_cpu_us() { return 0; }
extern "C" void port_log(const char*, ...) {}
namespace vgx {
bool LogEnabled() { return false; }
}
namespace {
enum { A_PNMTXIDX = 0, A_TEX0MTXIDX = 1, A_POS = 9, A_TEX0 = 13 };
enum { T_NONE = 0, T_DIRECT = 1, T_INDEX8 = 2, C_F32 = 4 };
}

// These markers surround, but are excluded from, the actual public entry count.
extern "C" __attribute__((noinline)) void position_count_begin() { asm volatile("" ::: "memory"); }
extern "C" __attribute__((noinline)) void position_count_end() { asm volatile("" ::: "memory"); }
extern "C" __attribute__((noinline)) void position_plan_begin() { asm volatile("" ::: "memory"); }
extern "C" __attribute__((noinline)) void position_plan_end() { asm volatile("" ::: "memory"); }

int main(int argc, char** argv) {
    using namespace vgx;
    if (argc != 2) return 2;
    FILE* bytes = std::fopen(argv[1], "wb");
    if (!bytes) return 2;
    unsigned cases = 0, eligibleVertices = 0, allVertices = 0;
    for (unsigned extras = 0; extras < 5; ++extras) {
        for (unsigned type = 0; type < 5; ++type) {
            for (unsigned components : {2u, 3u}) {
                if (extras >= 2 && (type != 3 || components != 3)) continue;
                for (unsigned count : {4u, 16u, 64u, 256u}) {
                    PlanCache cache{};
                    uint8_t vcd[26] = {};
                    VtxAttrFmt vat[8][26] = {};
                    vcd[A_POS] = extras == 4 ? T_INDEX8 : T_DIRECT;
                    vcd[A_TEX0] = extras == 1 ? T_DIRECT : T_NONE;
                    vcd[A_PNMTXIDX] = extras == 2 ? T_DIRECT : T_NONE;
                    vcd[A_TEX0MTXIDX] = extras == 3 ? T_DIRECT : T_NONE;
                    vat[0][A_POS] = {static_cast<uint8_t>(components == 3), static_cast<uint8_t>(type), 7};
                    vat[0][A_TEX0] = {1, C_F32, 0};
                    bool dirty = true;
                    GetPlanIn(cache, dirty, vcd, vat, 0, false);
                    dirty = true;
                    position_plan_begin();
                    const Plan& p = GetPlanIn(cache, dirty, vcd, vat, 0, false);
                    position_plan_end();
                    std::vector<uint8_t> src(count * p.streamSize + 32);
                    uint32_t random = 0x5A17C93Du ^ (type << 9);
                    for (uint8_t& b : src) { random = random * 1664525u + 1013904223u; b = random >> 24; }
                    std::vector<uint8_t> array(4096);
                    for (uint8_t& b : array) { random = random * 1664525u + 1013904223u; b = random >> 24; }
                    g.arrays[A_POS] = {array.data(), static_cast<uint32_t>(array.size()), 16, false};
                    const size_t size = count * p.layout.stride;
                    std::vector<uint8_t> dst(size + 32, 0xA5);
                    ArraySpans spans{};
                    // Warm the cached selector and byte lookup table before measurement.
                    DecodeVertices(p, src.data(), count, dst.data() + 16, &spans);
                    std::fill(dst.begin(), dst.end(), 0xA5);
                    position_count_begin();
                    DecodeVertices(p, src.data(), count, dst.data() + 16, &spans);
                    position_count_end();
                    for (unsigned i = 0; i < 16; ++i)
                        if (dst[i] != 0xA5 || dst[16 + size + i] != 0xA5) return 3;
                    if (std::fwrite(dst.data(), 1, dst.size(), bytes) != dst.size() ||
                        std::fwrite(&spans, 1, sizeof spans, bytes) != sizeof spans) return 4;
                    const uint32_t matrix = LastDecodedPosMtxMax();
                    if (std::fwrite(&matrix, 1, sizeof matrix, bytes) != sizeof matrix) return 4;
                    std::printf("case=%u extras=%u type=%u components=%u count=%u direct=%u eligible=%u stride=%u\n",
                                cases++, extras, type, components, count, p.direct, extras < 2, p.layout.stride);
                    allVertices += count;
                    if (extras < 2) eligibleVertices += count;
                }
            }
        }
    }
    if (std::fclose(bytes)) return 4;
    std::printf("cases=%u vertices=%u eligible_vertices=%u; synthetic warm entries, not frames or timing\n",
                cases, allVertices, eligibleVertices);
    return 0;
}
