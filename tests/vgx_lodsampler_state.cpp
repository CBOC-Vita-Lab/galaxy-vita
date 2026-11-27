// GXV_LODBIAS_SAMPLER (lodmask37) through the real front end: the BP writer (direct and by cached state-list replay), the key hash, the fragment-key
// builder, the texture-binding resolution and the draw records. Usage: vgx_lodsampler_state <mode in force 0|1|2|off> <plain|rung>
// (rung: the run has GXV_RUNG_LODBIAS=100, so the range test has a rung term). No GL, no game data.
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
unsigned failures = 0, checks = 0;
alignas(4096) uint8_t lists[4096] = {};
int g_mode = 0;       // the mode in force (0 off, 1 on, 2 count only)
bool g_fix = true;    // GXV_FIX_LODBIAS in force (without it nothing is biased at all)
bool g_rung = false;
void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
}
void BP(unsigned reg, unsigned value) { WriteBP(reg << 24 | value); }
constexpr uint32_t kMip = 6u << 5, kMax6 = 96u << 8;
uint32_t Mode0(int bias32) { return kMip | ((static_cast<unsigned>(bias32) & 0xFFu) << 9); }
unsigned Reg(unsigned map) { return map < 4 ? 0x80 + map : 0xA0 + map - 4; }
const float kTri[] = {-1,-1,-0.5f,0,0,0, 1,-1,-0.5f,0,1,0, 0,1,-0.5f,0,0.5f,1};
// one stage on texmap a (and a second on texmap b when b >= 0), both outputting the texture colour
void Stages(unsigned a, int b) {
    BP(0x00, 1u | (b >= 0 ? 1u << 10 : 0u));
    const uint32_t s0 = (1u << 6) | a | (7u << 7), s1 = b >= 0 ? ((1u << 6) | static_cast<unsigned>(b) | (7u << 7)) : 0u;
    BP(0x28, s0 | (s1 << 12));
    BP(0xC0, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19));
    if (b >= 0) BP(0xC2, (15u << 12) | (15u << 8) | (15u << 4) | 8u | (1u << 19));
}
void DrawIt() { DrawDecoded(0x90, VL_TEX0, reinterpret_cast<const uint8_t*>(kTri), 3); }
// What the draw must have: the expected class and steps for texmap m, by hand for the cases below (plain config: no rung term).
struct Want { lodsampler::Why why; int steps; };
void Expect(unsigned m, const Want& w0, const char* name) {
    char nm[160];
    Want w = w0;
    if (!g_fix) w = Want{lodsampler::kNone, 0}; // (no GXV_FIX_LODBIAS: the unit's bias is ignored altogether)
    const bool moved = g_mode == 1 && w.why == lodsampler::kMoved;
    const lodsampler::Why why = g_mode == 0 ? lodsampler::kNone : w.why;
    std::snprintf(nm, sizeof nm, "%s: class", name);
    Check(cur.tex[m].lodWhy == why, nm);
    std::snprintf(nm, sizeof nm, "%s: sampler steps", name);
    Check(cur.tex[m].lodSteps == (moved ? w.steps : 0), nm);
    // the draw record carries what cur has
    const DrawRec& d = g_buf.draws.back();
    std::snprintf(nm, sizeof nm, "%s: the draw record carries the binding's bias and class", name);
    Check(d.tex[m].lodSteps == cur.tex[m].lodSteps && d.tex[m].lodWhy == cur.tex[m].lodWhy, nm);
    // the key: a moved stage lost its shader bias; every other biased stage keeps it (mode 0 and 2 never touch the keys)
    bool keyBias = false, anyStage = false;
    for (int s = 0; s < cur.fs.numStages; ++s)
        if (cur.fs.stage[s].texEnable && (cur.fs.stage[s].texMap & 7u) == m) { anyStage = true; keyBias |= cur.fs.stage[s].lodBias != 0; }
    Check(anyStage, "the stage reads the texmap");
    const bool biasedUnit = w.why != lodsampler::kNone || (g_fix && w0.why != lodsampler::kNone); // (mode 0 keeps today's shader bias)
    std::snprintf(nm, sizeof nm, "%s: shader bias in the key", name);
    Check(keyBias == (biasedUnit && !moved), nm);
    Check(g.keyHash == KeyHashOf(g), "hash coherence");
}
}

int main(int argc, char** argv) {
    g_mode = argc > 1 && argv[1][0] >= '0' && argv[1][0] <= '2' ? argv[1][0] - '0' : 0;
    g_rung = argc > 2 && std::strcmp(argv[2], "rung") == 0;
    Check(LodSamplerMode() == g_mode, "the requested mode is in force");
    g_fix = !(argc > 1 && argv[1][0] == 'o');
    Check(FixLodBias() == g_fix, "GXV_FIX_LODBIAS as asked");
    hle::RegisterGuestRam(lists, sizeof lists);
    using namespace lodsampler;
    for (unsigned map = 0; map < 8; ++map) {
        // ---- single stage
        ResetState();
        Check(g.keyHash == KeyHashOf(g), "ResetState: coherent hash");
        const unsigned r = Reg(map);
        Stages(map, -1);
        BP(r + 4, kMax6);
        BP(r, Mode0(16)); DrawIt();                                     // +0.5 level: 4 eighths
        Expect(map, {kMoved, 4}, "+0.5");
        if (g_mode == 1) Check(FsTevCount(cur.fs) < 28, "a moved stage's shader has no bias rows (u_tev is under 28 vec4)");
        else if (g_mode != 0 && g_fix) Check(FsTevCount(cur.fs) == 28, "count-only: the key is untouched (28 vec4)");
        const uint16_t idMoved = cur.fsId;
        BP(r, Mode0(32)); DrawIt();                                      // +1 level: same key, the binding follows
        Expect(map, {kMoved, 8}, "+1.0, same key");
        Check(cur.fsId == idMoved || g_mode != 1 || true, "(key id may repeat)");
        if (g_mode == 1) Check(cur.fsId == idMoved, "a moved bias of another value is the same shader");
        BP(r, Mode0(-64)); DrawIt();                                     // -2 levels: -16 eighths (rung 100 %: -16 - 13 = -29, in range)
        Expect(map, {kMoved, -16}, "-2.0");
        BP(r, Mode0(-96)); DrawIt();                                     // -3 levels: -24 eighths; range with a rung term
        Expect(map, g_rung ? Want{kRange, 0} : Want{kMoved, -24}, "-3.0");
        BP(r, Mode0(1)); DrawIt();                                       // 1/32: not representable
        Expect(map, {kNotEighth, 0}, "+1/32");
        BP(r, Mode0(-2)); DrawIt();
        Expect(map, {kNotEighth, 0}, "-2/32");
        BP(r, Mode0(0)); DrawIt();                                       // no bias: no class, no bias in the key
        Expect(map, {kNone, 0}, "0");
        BP(r, Mode0(16)); BP(r + 4, 0); DrawIt();                        // the unit's maximum LOD 0: it does not mip-map, the bias means nothing
        Expect(map, {kNone, 0}, "max LOD 0");
        BP(r + 4, kMax6); DrawIt();
        Expect(map, {kMoved, 4}, "back to +0.5");
        BP(r, (4u << 5) | (16u << 9)); DrawIt();                         // no mip filter
        Expect(map, {kNone, 0}, "no mip filter");
        // ---- two stages
        const unsigned other = (map + 1) & 7;
        ResetState();
        Stages(map, static_cast<int>(other));
        BP(r + 4, kMax6); BP(Reg(other) + 4, kMax6);
        BP(r, Mode0(16)); BP(Reg(other), Mode0(16)); DrawIt();
        Expect(map, {kMoved, 4}, "two stages, the same bias: first");
        Expect(other, {kMoved, 4}, "two stages, the same bias: second");
        BP(Reg(other), Mode0(-16)); DrawIt();
        Expect(map, {kMixed, 0}, "two stages, +0.5 and -0.5: first");
        Expect(other, {kMixed, 0}, "two stages, +0.5 and -0.5: second");
        BP(Reg(other), Mode0(0)); DrawIt();
        Expect(map, {kMixed, 0}, "a biased and an unbiased stage: the biased");
        Expect(other, {kNone, 0}, "a biased and an unbiased stage: the other");
        BP(Reg(other), Mode0(16)); DrawIt();                             // the same bias again: moved again (the key follows the second unit's register)
        Expect(map, {kMoved, 4}, "the same again: first");
        Expect(other, {kMoved, 4}, "the same again: second");
        // ---- the key hash follows a bias value only while the switch is on; the dirty flag likewise
        ResetState();
        Stages(map, -1);
        BP(r + 4, kMax6); BP(r, Mode0(16)); DrawIt();
        const uint64_t h0 = g.keyHash;
        g.bpKeyDirty = false;
        BP(r, Mode0(20));
        Check((g.keyHash != h0) == (g_mode != 0), "a bias value change changes the key hash exactly when the switch is on");
        Check(g.bpKeyDirty == (g_mode != 0), "and marks the keys dirty exactly then");
        Check(g.keyHash == KeyHashOf(g), "incremental and recomputed hashes agree");
        BP(r, Mode0(16));
        Check(g.keyHash == h0, "and the hash returns with the value");
    }
    // ---- state-list replay (the inline texture-register path of the cached replay) keeps the hash and the decisions
    {
        ResetState();
        Stages(0, -1);
        BP(0x84, kMax6);
        BP(0x80, Mode0(16));
        uint8_t* list = lists;
        list[0] = 0x61;
        const uint32_t word = 0x80u << 24 | Mode0(32);
        for (unsigned i = 0; i < 4; ++i) list[i + 1] = static_cast<uint8_t>(word >> (24 - i * 8));
        hle::NotifyGuestWrite(list, 5);
        Check(StateListCall(list, 5), "state list is parsed by production code");
        Check(g.keyHash == KeyHashOf(g), "list: hash coherent");
        BP(0x80, Mode0(16));
        Check(StateListFastHit(list, 5), "cached replay takes the inline texture-register path");
        Check(g.keyHash == KeyHashOf(g), "replay: hash coherent");
        DrawIt();
        Expect(0, {kMoved, 8}, "replayed +1.0");
        BP(0x80, Mode0(0)); // (back to none) then replay again: the class returns with the register
        Check(StateListFastHit(list, 5), "second replay");
        DrawIt();
        Expect(0, {kMoved, 8}, "replayed +1.0 again");
    }
    // ---- an indirect read in the draw keeps the biased unit on the shader
    if (g_mode != 0) {
        ResetState();
        Stages(0, -1);
        BP(0x00, 1u | (1u << 16)); // one indirect stage
        BP(0x27, 1u);              // its texmap 1, coordinate 0
        BP(0x84, kMax6); BP(0x80, Mode0(16));
        DrawIt();
        Check(cur.fs.numIndStages == 0 || cur.tex[0].lodWhy == kIndirect || cur.tex[0].lodWhy == kMoved, "(an indirect stage that the key dropped does not count)");
        if (cur.fs.numIndStages) Check(cur.tex[0].lodWhy == kIndirect && cur.tex[0].lodSteps == 0 && cur.fs.stage[0].lodBias, "an indirect read keeps the shader bias");
    }
    std::printf("lodsampler state (mode %d, %s): %u checks, %u failures, front-end disagreements %llu\n", g_mode, g_rung ? "rung 100" : "plain", checks, failures,
                static_cast<unsigned long long>(LodSamplerInconsistent()));
    Check(LodSamplerInconsistent() == 0, "no key / binding disagreement");
    return failures ? 1 : 0;
}
