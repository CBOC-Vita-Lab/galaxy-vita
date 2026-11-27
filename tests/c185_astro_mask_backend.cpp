// Production resource planning/adoption on the Vita GL stub. Each case gets fresh cached knobs.
#define vglGetTexDataPointer c185_unused_const_tex_data_declaration
#define glGenFramebuffers c185_original_gen_framebuffers
#define glGenTextures c185_original_gen_textures
#define glDeleteFramebuffers c185_original_delete_framebuffers
#define glDeleteTextures c185_original_delete_textures
#include "asynclink_glshim.h"
#undef vglGetTexDataPointer
#undef glGenFramebuffers
#undef glGenTextures
#undef glDeleteFramebuffers
#undef glDeleteTextures
static bool c185FailFramebuffer, c185FailTexture, c185SentinelFramebuffer, c185SentinelTexture;
static unsigned c185InvalidDeletes;
inline void glGenFramebuffers(GLsizei n, GLuint* ids) {
    if (c185FailFramebuffer || c185SentinelFramebuffer) { for (GLsizei i = 0; i < n; ++i) ids[i] = c185SentinelFramebuffer ? ~0u : 0; }
    else c185_original_gen_framebuffers(n, ids);
}
inline void glGenTextures(GLsizei n, GLuint* ids) {
    if (c185FailTexture || c185SentinelTexture) { for (GLsizei i = 0; i < n; ++i) ids[i] = c185SentinelTexture ? ~0u : 0; }
    else c185_original_gen_textures(n, ids);
}
inline void glDeleteFramebuffers(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; ++i) c185InvalidDeletes += ids[i] == ~0u;
    c185_original_delete_framebuffers(n, ids);
}
inline void glDeleteTextures(GLsizei n, const GLuint* ids) {
    for (GLsizei i = 0; i < n; ++i) c185InvalidDeletes += ids[i] == ~0u;
    c185_original_delete_textures(n, ids);
}
inline void* vglGetTexDataPointer(unsigned) { return reinterpret_cast<void*>(1); }
inline void sceGxmFinish(void*) {}
#define C185_ASTRO_MASK 1
#define main c185_legacy_res_adopt_main
#include "vgx_res_adopt_backend.cpp"
#undef main

namespace {
constexpr uint32_t astro = vgx::prewarmres::Fnv("AstroGalaxy");
constexpr uint32_t other = vgx::prewarmres::Fnv("EggStarGalaxy");

void Env(const char* key, const char* value) { Require(setenv(key, value, 1) == 0, "set test environment"); }

void Setup(const std::string& test) {
    Env("GXV_PREWARM_RES", test == "prewarm-off" ? "0" : "2");
    Env("GXV_PREWARM_RES_MAX", test == "unknown-have" || test == "eligibility-replan" ? "6" : "1");
    Env("GXV_PREWARM_RES_MIB", test == "bytecap" ? "1" : "16");
    Env("GXV_PREWARM_RES_SEED", "0");
    Env("GXV_PREWARM_RES_UP", "0");
    Env("GXV_PREWARM_RES_RESERVE_MIB", "28");
    Env("GXV_STAGEWARM", "0");
    Env("GXV_AUTOQ", "0");
    Env("GXV_KEEP_RT", "0");
    Env("GXV_RES_ADOPT", "1");
    Env("GXV_VRAM_GUARD", "0");
    Env("GXV_RT_DEFER", "3");
    Env("GXV_SMOOTHSTART", "1");
    Env("GXV_SMOOTHSTART_MS", "400");
    Env("GXV_MIDLOAD_HOLD", "0");
    Env("GXV_HRW", test == "hrw-off" ? "0" : "5");
    Env("GXV_HRW_STAGE", test == "stage-disabled" ? "0" : "1");
    Env("GXV_HRW_WARP_DEPTH", test == "depth-off" ? "0" : "4");
    Env("GXV_HRW41_DEPTH", test == "box-only" || test == "consumer-disabled" ? "0" : "1");
    Env("GXV_HRW41_BOXKEY", test == "box-only" ? "1" : "0");
    Env("GXV_HRW41_MASK_DIV", test == "half" || test == "bytecap" ? "2" : "4");
    Env("GXV_PICTURE_MODE", test == "quality" ? "30" : "60");
    unsetenv("GXV_PREWARM_RES_FILE");
    unsetenv("GXV_AUTOQ_VRAM_KIB");
    unsetenv("GXV_C185_ASTRO_MASK_WARM");
    if (test == "off") Env("GXV_C185_ASTRO_MASK_WARM", "0");
    // (fix31) C185's hold rule for an unknown VRAM reading is behind GXV_C185_HOLD_UNKNOWN_VRAM=1 (default 0 = C184's): these cases test it on; the "-c184"
    // cases test the default
    if (test.size() > 5 && test.compare(test.size() - 5, 5, "-c184") == 0) unsetenv("GXV_C185_HOLD_UNKNOWN_VRAM");
    else Env("GXV_C185_HOLD_UNKNOWN_VRAM", "1");
    Require(Init(32, 32), "initialize real backend against GL stub");
    g_pool.clear(); g_resPre.clear(); g_rungPre.clear(); g_resReq.clear();
    g_resBook = vgx::prewarmres::Book{};
    g_rc = ResCensus{};
    g_resPendingN = 0;
    g_resPendingStage = 0;
    g_astroMaskPreTex = 0;
    scene = loading = g_stageTarget = g_stageRecKey = astro;
    quiet = 2;
    g_screenLive = false;
    g_inPass = false;
    g_pass = -1;
    g_rtW = 960; g_rtH = 544;
    g_efbW = 640; g_efbH = 456;
    g_efbSamples = 1;
    g_ovX = g_ovY = 0;
    g_presents = 100;
    warp31::g_w.progMask = 7001;
    warp31::g_w.prog[warp31::kWarpD41].prog = test == "box-only" ? 0 : 7002;
    warp31::g_w.prog[warp31::kWarpBoxKey].prog = 7003;
    if (test == "no-consumer" || test == "eligibility-replan") {
        warp31::g_w.prog[warp31::kWarpD41].prog = 0;
        warp31::g_w.prog[warp31::kWarpBoxKey].prog = 0;
    }
    if (test == "no-mask-program") warp31::g_w.progMask = 0;
    if (test == "non-astro") scene = loading = g_stageTarget = g_stageRecKey = other;
    if (test == "msaa") g_efbSamples = 2;
    if (test == "overscan-x") g_ovX = 8;
    if (test == "overscan-y") g_ovY = 8;
    if (test == "small-width") g_rtW = 63;
    if (test == "small-height") g_rtH = 63;
    if (test == "half") { g_rtW = 853; g_rtH = 483; }
    if (test == "bytecap") { g_rtW = 1920; g_rtH = 1088; }
}

bool RejectCase(const std::string& test) {
    return test == "off" || test == "prewarm-off" || test == "quality" || test == "non-astro" || test == "msaa" ||
        test == "overscan-x" || test == "overscan-y" || test == "small-width" || test == "small-height" ||
        test == "no-consumer" || test == "consumer-disabled" || test == "no-mask-program" ||
        test == "hrw-off" || test == "depth-off" || test == "stage-disabled";
}

void NoHold() {
    Require(g_resPendingN == 0 && smooth26::Pending() == 0, "skipped/finished plan publishes no pending work immediately");
    Require(!smooth26::HoldPoll(1000.0) && !smooth26::g.holding && smooth26::g.holdMs == 0,
            "skipped/finished plan does not start or waste smooth-start hold");
}

ResPre BuiltMask() {
    ResPreStep();
    Require(g_rc.planned == 1 && g_rc.targeted == 1 && g_rc.made == 1 && g_rc.frames == 1,
            "one closed opportunity builds the priority exact mask within count cap");
    Require(g_resPre.size() == 1 && g_resPre.front().astroMask, "targeted premade is marked, not a generic target");
    const auto r = g_resPre.front();
    Require(g_astroMaskPreTex == r.tex, "single reserved texture handle names targeted spare");
    Require(r.stage == astro && r.picW == g_rtW && r.picH == g_rtH && r.w == (g_rtW / warp31::MaskDivEnv() & ~1) &&
            r.h == (g_rtH / warp31::MaskDivEnv() & ~1), "targeted ownership and exact runtime-rounded dimensions");
    Require(g_pool.size() == 1 && g_pool.front().attached == r.tex && !g_pool.front().depthTex && !g_pool.front().depthRb,
            "mask has a premade color FBO but no explicit depth texture/renderbuffer");
    NoHold();
    return r;
}

void PlanLimits() {
    using namespace vgx::prewarmres;
    Item hint; hint.w = 240; hint.h = 136; hint.picW = 960; hint.picH = 544;
    Book book;
    Item recorded = hint; recorded.w = 200; recorded.h = 100;
    Require(book.Note(astro, recorded), "record competing learned item");
    for (unsigned cap : {1u, 2u, 6u, kMaxItems}) {
        Plan p;
        BuildPlan(&book, astro, 2, 960, 544, cap, 16u << 20, p, 640, 456, true, 0, 0, &hint);
        Require(p.n <= cap && p.n && p.targeted == 1 && p.item[0] == hint && p.item[0].depth == 0,
                "targeted hint is first under every existing item limit without explicit depth");
        uint64_t bytes = 0;
        for (unsigned i = 0; i < p.n; ++i) bytes += ItemBytes(p.item[i]);
        Require(bytes <= (16u << 20), "priority plan retains byte limit");
    }
    Plan shortPlan, exactPlan;
    BuildPlan(&book, astro, 2, 960, 544, 1, ItemBytes(hint) - 1, shortPlan, 640, 456, false, 0, 0, &hint);
    Require(!shortPlan.targeted, "priority hint cannot bypass byte cap");
    BuildPlan(&book, astro, 2, 960, 544, 1, ItemBytes(hint), exactPlan, 640, 456, false, 0, 0, &hint);
    Require(exactPlan.n == 1 && exactPlan.targeted == 1 && exactPlan.item[0] == hint, "exact byte cap accepts mask");
}

void AdoptMask(const ResPre& r) {
    // An identically sized generic texture and rung spare must not be stolen by this new path.
    g_resPre.insert(g_resPre.begin(), ResPre{r.w, r.h, 8001, r.until});
    g_rungPre.push_back(RungPre{r.w, r.h, 8002});
    CmdBuf buf;
    buf.uni.assign(16, 0.f); buf.uni[0] = buf.uni[5] = 1; buf.uni[10] = -1.01f; buf.uni[11] = -1; buf.uni[14] = -64;
    buf.draws.emplace_back(); buf.draws.back().ublock[UB_PROJ] = 0;
    buf.cmds.push_back(Cmd{CMD_DRAW, 0});
    g_pxBuf = &buf;
    warp31::g_w.armedId = 42;
    const unsigned made = g_poolMade, rungHits = g_rungPreHits;
    Require(warp31::MaskSnap(), "real MaskSnap renders into targeted premade texture");
    Require(warp31::g_w.texMask == r.tex && warp31::g_w.maskOk && warp31::g_w.maskId == 42,
            "MaskSnap adopts the exact tagged texture once");
    Require(g_poolMade == made && g_resPre.size() == 1 && g_resPre[0].tex == 8001 &&
            g_rungPre.size() == 1 && g_rungPre[0].tex == 8002 && g_rungPreHits == rungHits,
            "adoption preserves generic/rung spares and creates no new FBO");
    Require(TakeResPre(r.w, r.h, true) == 0, "targeted texture cannot be taken twice");
    g_pxBuf = nullptr;
}
}

int main(int argc, char** argv) {
    Require(argc == 2, "one isolated test case requested");
    const std::string test = argv[1];
    const std::set<std::string> cases = {"default", "off", "prewarm-off", "no-consumer", "consumer-disabled", "no-mask-program", "quality", "non-astro", "hrw-off", "depth-off", "stage-disabled", "msaa", "overscan-x", "overscan-y", "small-width", "small-height", "box-only", "half", "limits", "bytecap", "adopt", "owned", "reservation", "generic-same-mask", "expiry", "closed-only", "drs", "stage-change", "eligibility-change", "eligibility-replan", "unknown", "unknown-overfree", "unknown-have", "unknown-c184", "unknown-have-c184", "lowmem", "allocfail", "fbo-fail", "tex-fail", "fbo-sentinel", "tex-sentinel"};
    Require(cases.count(test) == 1, "known test case requested");
    Setup(test);
    int w = -1, h = -1;
    const bool eligible = warp31::EarlyMaskSize(g_stageTarget, w, h);
    if (test == "eligibility-replan") {
        Require(!eligible, "no initial mask consumer for generic-only plan");
        ResPreStep(); ResPreStep();
        Require(g_rc.planned == 2 && g_rc.targeted == 0 && g_rc.made == 2 && g_resPre.size() == 2,
                "generic targets completed before consumer eligibility changes");
        NoHold();
        const auto generic = g_resPre;
        for (unsigned toggle = 0; toggle < 3; ++toggle) {
            const bool on = toggle != 1;
            warp31::g_w.prog[warp31::kWarpD41].prog = on ? 7002 : 0;
            const auto made = g_rc.made;
            ResPreStep();
            Require(g_rc.targeted == static_cast<unsigned>(on) && g_rc.planned == (on ? 3u : 2u) &&
                    g_rc.made == made + static_cast<unsigned>(on),
                    "same-stage consumer change only creates newly needed mask, not completed generic targets");
            for (const auto& prior : generic) {
                bool same = false;
                for (const auto& current : g_resPre) same = same || (current.tex == prior.tex && !current.astroMask);
                Require(same, "completed generic target identities survive eligibility-only replans");
            }
            NoHold();
        }
    } else if (RejectCase(test)) {
        Require(!eligible && w == 0 && h == 0, "ineligible configuration produces no exact mask hint");
        ResPreStep();
        Require(g_rc.targeted == 0, "ineligible configuration leaves generic plan unchanged");
        for (const auto& r : g_resPre) Require(!r.astroMask, "ineligible plan creates no targeted texture");
    } else {
        Require(eligible && w == (g_rtW / warp31::MaskDivEnv() & ~1) && h == (g_rtH / warp31::MaskDivEnv() & ~1),
                "eligible configuration returns exact mask dimensions");
        if (test == "limits") {
            PlanLimits();
            vgx::prewarmres::Item competing; // Runtime priority must also displace a recorded item under MAX=1.
            competing.w = 200; competing.h = 100; competing.picW = g_rtW; competing.picH = g_rtH;
            Require(g_resBook.Note(astro, competing), "record runtime competing item");
            BuiltMask();
        } else if (test == "bytecap") {
            ResPreStep();
            Require(g_rc.targeted == 0 && g_rc.planned <= 1, "oversized half mask remains under runtime byte/count cap");
            for (const auto& r : g_resPre) Require(!r.astroMask, "oversized mask not allocated");
        } else if (test == "owned") {
            warp31::g_w.texMask = 9001; warp31::g_w.maskW = w; warp31::g_w.maskH = h;
            ResPreStep();
            Require(g_rc.targeted == 1 && g_rc.had == 1 && g_rc.made == 0 && g_resPre.empty() && g_pool.empty(),
                    "repeated visit reuses already-owned exact mask without allocating a spare");
            Require(warp31::EarlyMaskSize(astro, w, h), "owned mask does not disable policy eligibility");
            NoHold();
            ResPreStep();
            Require(g_rc.frames == 2 && g_rc.had == 1 && g_rc.made == 0, "completed owned-mask item is not reprocessed");
            NoHold();
        } else if (test == "generic-same-mask") {
            PreMade(w, h, 9001, false);
            ResPreStep();
            Require(g_rc.had == 1 && g_rc.made == 0 && g_resPre.size() == 1 && g_resPre[0].tex == 9001 &&
                    g_resPre[0].astroMask && g_resPre[0].stage == astro && g_resPre[0].picW == g_rtW &&
                    g_resPre[0].picH == g_rtH && g_astroMaskPreTex == 9001,
                    "same-size generic spare is promoted with exact ownership and original identity, not duplicated");
            const auto promoted = g_resPre[0];
            AdoptMask(promoted);
            NoHold();
        } else if (test == "unknown-c184" || test == "unknown-have-c184") {
            // (fix31) the default, C184's rule: an unknown reading stops the step with the item still pending, so the smooth-start hold waits for it
            if (test == "unknown-have-c184") PreMade(240, 191, 9001, false);
            c185VramTotal = 0;
            ResPreStep();
            Require(g_rc.made == 0 && g_rc.unknownVram == 1 && g_resPendingN >= 1 && g_resPendingStage == astro && smooth26::Pending() >= 1,
                    "C184 rule (default): an unknown VRAM reading leaves the item pending and the hold counts it");
            if (test == "unknown-have-c184") Require(g_rc.had == 0 && g_resPre.size() == 1, "C184 rule: the step stops at the first unknown reading");
            const unsigned pending = g_resPendingN;
            g_screenLive = true;
            ResPreStep();
            Require(g_resPendingN == pending, "C184 rule: an early return leaves the last published pending count");
        } else if (test == "unknown-have") {
            PreMade(240, 191, 9001, false);
            c185VramTotal = 0;
            ResPreStep();
            Require(g_rc.planned == 3 && g_rc.had == 1 && g_rc.unknownVram == 2 && g_rc.made == 0 &&
                    g_resPre.size() == 1 && g_resPre[0].tex == 9001,
                    "unknown VRAM skips every missing item but correctly counts an existing later item");
            NoHold();
        } else if (test == "unknown" || test == "unknown-overfree" || test == "lowmem" || test == "allocfail" || test == "fbo-fail" || test == "tex-fail" || test == "fbo-sentinel" || test == "tex-sentinel") {
            if (test == "unknown") c185VramTotal = 0;
            if (test == "unknown-overfree") c185VramFree = c185VramTotal + 1;
            if (test == "lowmem") c185VramFree = 8u << 20;
            if (test == "allocfail") { g_simFailAt = 1; g_simChecks = 0; }
            if (test == "fbo-fail") c185FailFramebuffer = true;
            if (test == "tex-fail") c185FailTexture = true;
            if (test == "fbo-sentinel") c185SentinelFramebuffer = true;
            if (test == "tex-sentinel") c185SentinelTexture = true;
            ResPreStep();
            Require(g_rc.frames == 1 && g_rc.made == 0 && g_resPre.empty(), "failed/skipped opportunity counted without premade allocation");
            if (test == "unknown" || test == "unknown-overfree") Require(g_rc.unknownVram == 1, "unknown VRAM explicitly counted as skipped");
            if (test == "lowmem") Require(g_rc.failedMem == 1, "low VRAM explicitly counted as skipped");
            if (test == "allocfail" || test == "fbo-fail" || test == "tex-fail" || test == "fbo-sentinel" || test == "tex-sentinel") Require(g_rc.failed == 1, "storage/FBO/texture allocation failure explicitly counted");
            Require(c185InvalidDeletes == 0, "invalid sentinel is never passed to GL deletion");
            for (const auto& late : g_lateTex) Require(late.id != ~0u, "invalid sentinel is never queued for deferred texture deletion");
            for (const auto& f : g_pool) Require(f.fbo != ~0u && f.attached != ~0u, "invalid sentinel is never registered in resource pool");
            NoHold();
            ResPreStep();
            Require(g_rc.frames == 2 && g_rc.made == 0, "closed opportunities counted even with no successful allocation");
            NoHold();
        } else {
            if (test == "closed-only") {
                loading = 0; quiet = 1;
                ResPreStep();
                Require(g_rc.frames == 0 && g_rc.made == 0 && g_resPre.empty(), "open preparation phase does not count as closed opportunity");
                loading = astro;
            }
            const auto r = BuiltMask();
            if (test == "adopt") AdoptMask(r);
            if (test == "reservation") {
                Require(TakeResPre(r.w, r.h) == 0 && g_resPre.size() == 1, "generic texture take cannot steal reserved mask");
                const auto idx = AcquireFbo(r.w, r.h, 9001, false);
                Require(idx >= 0 && g_pool.size() == 2 && g_pool[0].attached == r.tex && g_resPre.size() == 1 &&
                        g_resPre[0].tex == r.tex && g_rc.adopted == 0,
                        "generic FBO adoption cannot steal reserved mask object");
                g_rtW = 848;
                Require(TakeResPre(r.w, r.h, true) == 0, "targeted take rejects stale picture dimensions");
                g_rtW = 960; g_stageTarget = other;
                Require(TakeResPre(r.w, r.h, true) == 0, "targeted take rejects stale target stage");
                g_stageTarget = astro;
                Require(TakeResPre(r.w, r.h, true) == r.tex && !g_astroMaskPreTex, "only exact current mask take releases reserved handle");
            }
            if (test == "expiry") {
                g_resPre[0].until = g_presents;
                ResPreExpire();
                Require(g_resPre.empty() && !g_astroMaskPreTex && !(g_pool[0].held & PoolHeld::PRIME) && !g_pool[0].keepUntil,
                        "expiry releases reserved texture and PRIME lifetime");
            }
            if (test == "drs" || test == "stage-change" || test == "eligibility-change") {
                g_screenLive = true;
                if (test == "drs") { g_rtW = 848; g_rtH = 480; }
                if (test == "stage-change") g_stageTarget = other;
                if (test == "eligibility-change") g_efbSamples = 2;
                ResPreStep();
                Require(g_resPre.empty() && !ResPreIs(r.tex) && !g_astroMaskPreTex &&
                        !(g_pool[0].held & PoolHeld::PRIME) && !g_pool[0].keepUntil,
                        "stale tagged mask and PRIME lifetime cancelled before live/phase early return");
                NoHold();
                if (test == "drs") {
                    g_screenLive = false;
                    ResPreStep();
                    Require(g_resPre.size() == 1 && g_resPre[0].astroMask && g_resPre[0].w == 212 && g_resPre[0].h == 120 &&
                            g_resPre[0].picW == 848 && g_resPre[0].picH == 480 && g_resPre[0].tex != r.tex,
                            "DRS rebuilds only the exact new-size mask");
                    NoHold();
                }
            }
        }
    }
    std::printf("PASS C185 Astro mask: %s\n", test.c_str());
    return 0;
}

// (tests42: vgx_draw.cpp, not linked here, owns these; the back end calls them)
namespace vgx { int TailFoldMode() { return 0; } int HrwMode() { return 0; } }
