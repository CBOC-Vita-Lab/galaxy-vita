// Real backend, deferred queue, shader/program demand and RGBA rendering on Mesa.
#define main original_warm_variants_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "vgx_warm_variants.cpp"
#pragma clang diagnostic pop
#undef main

namespace {
unsigned scene = 7, quiet = 0, splashState = 0;
unsigned sceneReads = 0, quietReads = 0, splashReads = 0, clockReads = 0, programCreates = 0;
bool failProgram = false;
}
#if WARM_HOOKS & 1
extern "C" std::uint32_t port_scene_key() { ++sceneReads; return scene; }
#endif
#if WARM_HOOKS & 2
extern "C" int port_screen_quiet() { ++quietReads; return quiet; }
#endif
#if WARM_HOOKS & 4
extern "C" int port_vgx_vita_splash_active() { ++splashReads; return splashState != 0; }
#endif
// Observe the existing backend clock boundary; no substitute warm scheduler.
std::chrono::steady_clock::time_point std::chrono::steady_clock::now() noexcept {
    return time_point(std::chrono::duration_cast<duration>(std::chrono::milliseconds(1000u + ++clockReads)));
}
extern "C" GLuint __real_glCreateProgram();
extern "C" GLuint __wrap_glCreateProgram() {
    ++programCreates;
    if (failProgram) { failProgram = false; return 0; }
    return __real_glCreateProgram();
}
namespace {
using namespace vgx;
using namespace vgx::backend;
std::vector<WarmItem> queued;
void Enqueue() {
    static ShaderSource vertex, fragment;
    vertex.id = 3; fragment.id = 4; fragment.fragment = true;
    vertex.text = "attribute vec4 a_pos; void main(){gl_Position=a_pos;}";
    fragment.text = "precision mediump float; void main(){gl_FragColor=vec4(0.25,0.75,0.5,1.0);}";
    CmdBuf b; b.shaders = {&vertex, &fragment}; b.variants.push_back({3,4,0,0xc00});
    for (Cmd c : std::array<Cmd,4>{{{CMD_SHADER,0},{CMD_SHADER,1},{CMD_PROGRAM,(3u<<16)|4u},{CMD_VARIANT,0}}}) DeferWarm(b,c);
    queued.assign(g_warm.begin(), g_warm.end());
    Require(g_vsSrc[3] == &vertex && g_fsSrc[4] == &fragment && g_vsPending[3] && g_fsPending[4], "deferred shaders registered before optional execution");
    SetWarmBudget(1000000); g_serial = 20;
    clockReads = sceneReads = quietReads = splashReads = 0;
}
bool SameQueue() {
    if (g_warm.size() != queued.size()) return false;
    for (size_t i = 0; i < queued.size(); ++i) {
        const auto& a = g_warm[i]; const auto& b = queued[i];
        if (a.type != b.type || a.arg != b.arg || a.src != b.src || a.var.vsId != b.var.vsId || a.var.fsId != b.var.fsId ||
            a.var.layoutMask != b.var.layoutMask || a.var.blendKey != b.var.blendKey) return false;
    }
    return true;
}
void Blocked(const char* why) {
    const auto compiles = g_sw.compiles, links = g_sw.links, draws = actualDraws, creates = programCreates;
    const auto slices = g_warmSlices.load(), reads = clockReads;
    RunWarm();
    Require(SameQueue() && WarmPending() == 23 && WarmTotal() == 23, why);
    Require(g_sw.compiles == compiles && g_sw.links == links && actualDraws == draws && programCreates == creates,
        "blocked optional work does not compile/link/draw/allocate");
    Require(g_warmSlices.load() == slices && clockReads == reads, "blocked optional work does not start a slice or read its clock");
}
void Retire() {
    RunWarm();
    Require(g_warm.empty() && WarmPending() == 0 && WarmTotal() == 23, "allowed queue retires all shader/program/variant weighted units");
    Require(g_programs.at((3u<<16)|4u).ok && !g_vsPending[3] && !g_fsPending[4], "allowed queue produces usable real shaders/program");
}
void Demand() {
    auto b = Triangle(3,4); Draw(b);
    const auto pixels = Read(); const size_t center = (16*32+16)*4;
    Require(pixels[center] == 64 && pixels[center+1] == 191 && pixels[center+2] == 128 && pixels[center+3] == 255,
        "foreground demand compiles/links registered source and renders expected RGBA");
}
}
int main(int argc, char** argv) {
    Require(argc == 2, "select warm gameplay gate scenario");
    const std::string mode = argv[1];
    Context(); Require(vgx::backend::Init(32,32), "real renderer initializes");
#if VGX_COLDTRACE
    Require(vgx::coldtrace::g_recorder.Arm(TraceClock, TraceThread, 1), "existing RAM trace arms for enabled-scope compatibility");
#endif
    Enqueue();
    if (mode == "legacy") {
        Require((WARM_HOOKS & 3) != 3, "legacy scenario omits a scene/quiet export");
        Retire();
    } else if (mode == "unknown") {
        scene = 0; Retire();
    } else if (mode == "startup-gl" || mode == "startup-yielded") {
        Require((WARM_HOOKS & 4) != 0, "startup export present");
        splashState = mode == "startup-gl" ? 1 : 2;
        Retire();
        Require(splashReads > 0, "known-key/nonquiet startup reaches the existing splash bypass");
    } else if (mode == "blocked" || mode == "missing-splash") {
        Blocked("known-stage nonquiet execution preserves optional queue identity/order/units");
        const auto reads = clockReads; WarmIdle();
        Require(SameQueue() && clockReads == reads, "idle pump cannot bypass optional suspension");
        quiet = 1; Retire();
    } else if (mode == "demand") {
        Blocked("demand scenario first leaves optional work pending");
        const auto compiles = g_sw.compiles, links = g_sw.links;
        Demand();
        Require(SameQueue() && WarmPending() == 23 && g_sw.compiles == compiles + 2 && g_sw.links == links + 1, "demand does not retire or cancel optional queue work");
        const auto before = Read(); quiet = 1; Retire();
        Require(Read() == before && g_sw.compiles == compiles + 2 && g_sw.links == links + 1, "quiet retirement after demand preserves pixels and cached shader/program work");
    } else if (mode == "guards") {
        quiet = 1; SetWarmBudget(0); Blocked("zero budget preserves optional queue");
        Require(sceneReads == 0 && quietReads == 0 && splashReads == 0, "zero budget remains before state-hook reads");
        SetWarmBudget(1000000); g_warmBackoffLeft = 3; quiet = 0;
        Blocked("active gameplay leaves backoff pending"); Require(g_warmBackoffLeft == 3, "suspension does not consume backoff");
        quiet = 1; RunWarm(); Require(SameQueue() && g_warmBackoffLeft == 2 && clockReads == 0, "quiet respects the existing backoff countdown");
        g_warmBackoffLeft = 0; g_glAllocFailSerial = g_serial; Blocked("existing allocation failure blocks optional work");
        g_glAllocFailSerial = ~uint64_t{0}; g_warmAllocFailSerial = g_serial; Blocked("warm failure serial guard remains authoritative");
        g_warmAllocFailSerial = ~uint64_t{0}; Retire();
    } else if (mode == "failure") {
        quiet = 1; failProgram = true; RunWarm();
        Require(WarmPending() == 3 && g_warm.size() == 2 && g_warm.front().type == CMD_PROGRAM && g_warmAllocFailSerial == g_serial,
            "real failed allocation retains the program/variant queue for retry");
        quiet = 0; const auto creates = programCreates; RunWarm();
        Require(WarmPending() == 3 && programCreates == creates, "blocked failure state makes no extra optional attempt");
        Demand(); Require(WarmPending() == 3, "foreground demand keeps its bounded same-serial allocation attempt");
        quiet = 1; RunWarm(); Require(WarmPending() == 3, "warm failure stays deferred even after demand recovered");
        ++g_serial; RunWarm(); Require(WarmPending() == 0 && g_warm.empty(), "quiet next serial retires recovered optional work");
    } else {
        Require(false, "known scenario");
    }
#if VGX_COLDTRACE
    vgx::coldtrace::g_recorder.RequestFreeze();
    const auto captured = vgx::coldtrace::g_recorder.Frozen();
    Require(captured.data && captured.clockErrors == 0 && captured.foreignDropped == 0 && vgx::coldtrace::g_recorder.ScopeMisuses() == 0,
        "enabled trace compatibility retains valid clocks and balanced scopes");
#endif
    std::printf("PASS: actual warm gameplay gate %s hooks=%d diagnostics=%d coldtrace=%d\n", argv[1], WARM_HOOKS, PORT_RUNTIME_DIAGNOSTICS, VGX_COLDTRACE);
}
