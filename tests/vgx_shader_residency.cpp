#define main quiet_fixture_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#define VGX_TEST_NATIVE_CAPACITY
#include "vgx_backend_quiet.cpp"
#pragma clang diagnostic pop
#undef main
#include <unordered_set>

namespace {
std::unordered_set<GLuint> livePrograms;
unsigned creates = 0, deletes = 0, rejected = 0, queries = 0;
unsigned scene = 7, quiet = 1, splash = 0;
void Prepare(unsigned count) {
    static vgx::ShaderSource vs, fs;
    vs.id = 3; fs.fragment = true;
    vs.text = "attribute vec4 a_pos; void main(){gl_Position=a_pos;}";
    fs.text = "precision mediump float; void main(){gl_FragColor=vec4(0.25,0.75,0.5,1.0);}";
    using namespace vgx::backend;
    g_vs.resize(4); g_vs[3] = Shader(GL_VERTEX_SHADER, vs.text.c_str());
    g_vsSrc.resize(4); g_vsSrc[3] = &vs;
    const GLuint fragment = Shader(GL_FRAGMENT_SHADER, fs.text.c_str());
    g_fs.assign(count, fragment); g_fsSrc.assign(count, &fs);
}
void Queue(unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        vgx::CmdBuf b; b.variants.push_back({3, static_cast<uint16_t>(i), 0, 0xc00});
        vgx::backend::DeferWarm(b, {vgx::CMD_VARIANT, 0});
    }
}
void CheckFifo(unsigned total) {
    using namespace vgx::backend;
    Require(WarmPending() == g_warm.size() && WarmTotal() == total, "weighted backlog remains honest");
    unsigned next = total - WarmPending();
    for (const auto& item : g_warm)
        Require(item.type == vgx::CMD_VARIANT && item.var.fsId == next++, "retained head identities remain FIFO");
}
void DemandPixels(unsigned id) {
    using namespace vgx::backend;
    auto b = Triangle(); b.draws[0].vsId = 3; b.draws[0].fsId = static_cast<uint16_t>(id);
    UploadStream(b); Draw(b, b.draws[0]);
    const auto p = Read(); const size_t c = (16 * 32 + 16) * 4;
    Require(p[c] == 64 && p[c+1] == 191 && p[c+2] == 128 && p[c+3] == 255,
            "fresh demand renders exact RGBA under warm pressure");
}
}
extern "C" uint32_t port_scene_key() { return scene; }
extern "C" int port_screen_quiet() { return quiet; }
extern "C" int port_vgx_vita_splash_active() { return splash; }
extern "C" unsigned vglGetFreeProgramSlots() { ++queries; return 1024u - unsigned(livePrograms.size()); }
extern "C" GLuint __real_glCreateProgram();
extern "C" GLuint __wrap_glCreateProgram() {
    if (livePrograms.size() == 1024) { ++rejected; return 0; }
    const auto id = __real_glCreateProgram();
    Require(id != 0, "Mesa allocates actual program");
    Require(livePrograms.insert(id).second, "native name live set unique"); ++creates; return id;
}
extern "C" void __real_glDeleteProgram(GLuint);
extern "C" void __wrap_glDeleteProgram(GLuint id) {
    Require(id && id != ~0u, "invalid name never deleted");
    Require(livePrograms.erase(id) == 1, "delete owns live native name"); ++deletes;
    __real_glDeleteProgram(id);
}
int main(int argc, char** argv) {
    Require(argc == 2, "one scenario"); const std::string mode = argv[1];
    const bool small = mode == "small-limit", utilities = mode == "utility-pressure";
    if (mode == "ground-burst") {
        // The Observatory ground (2 Nov): a full warm-up, then an area whose draws need more programs in one frame than the
        // warm-up's reserve leaves. Every one of those draws must get its program in that same frame (GXV_DEMAND_RECLAIM).
        setenv("GXV_MAXPROGRAMS", "900", 1);
        Context(); Require(vgx::backend::Init(32,32), "actual backend initialized"); Prepare(1500);
        using namespace vgx::backend;
        g_serial = 1000; SetWarmBudget(1000000); Queue(1280); RunWarm();
        Require(WarmPending() > 0, "warm-up stops at its reserve");
        ++g_serial;
        unsigned ok = 0;
        for (unsigned i = 1100; i < 1500; ++i) {
            vgx::backend::Program& p = GetProgram(3, static_cast<uint16_t>(i));
            if (p.ok) { ++ok; p.usedSerial = g_serial; p.drawSerial = g_serial; }
        }
        const char* want = getenv("GXV_DEMAND_RECLAIM");
        if (want && want[0] == '0') {
            Require(ok < 400, "old policy: a burst beyond the reserve skips draws (reproduces the missing ground)");
        } else {
            Require(ok == 400, "every draw of the burst gets its program in the same frame");
            Require(rejected == 0 || livePrograms.size() <= 1024, "native table never over-committed");
            DemandPixels(1499);
        }
        char output[256]; const int n = std::snprintf(output, sizeof output, "PASS mode=ground-burst ok=%u/400 creates=%u deletes=%u rejected=%u live=%zu\n",
                                                     ok, creates, deletes, rejected, livePrograms.size());
        Require(n > 0 && ::write(STDOUT_FILENO, output, n) == n, "write result");
        return 0;
    }
    Require(small || utilities || mode == "quiet-pressure" || mode == "startup-pressure" || mode == "active" || mode == "lifecycle-no-demand" || mode == "demand-render",
            "known residency scenario");
    if (mode == "startup-pressure") { quiet = 0; splash = 1; }
    if (mode == "active") quiet = 0;
    setenv("GXV_MAXPROGRAMS", small ? "1" : "900", 1);
    Context(); Require(vgx::backend::Init(32,32), "actual backend initialized"); Prepare(1500);
    using namespace vgx::backend;
    std::vector<GLuint> utilityIds;
    if (utilities) for (unsigned i = 0; i < 200; ++i) utilityIds.push_back(__wrap_glCreateProgram());
    g_serial = 1000; SetWarmBudget(1000000); const unsigned total = small ? 4 : 1280;
    Queue(total); RunWarm(); CheckFifo(total);
    if (small) {
        Require(WarmPending() == 0 && g_programs.size() == 4, "soft map target1 must not cap native warm admission");
    } else if (mode == "active") {
        Require(WarmPending() == total && g_programs.empty(), "active gameplay retains entire optional backlog");
        Require(queries == 0, "gameplay gate does not even query native occupancy");
        Require(GetProgram(3,1499).ok, "active demand remains available"); DemandPixels(1499);
    } else {
        if (mode != "demand-render")
            Require(rejected == 0 && livePrograms.size() <= 900, "optional warming reserves124 actual native names including utilities");
        Require(WarmPending() > 0, "pressure yields honest retained queue");
        const unsigned before = WarmPending(), beforeCreates = creates, beforeQueries = queries;
        for (unsigned i = 0; i < 8; ++i) WarmIdle();
        Require(WarmPending() == before && creates == beforeCreates && queries == beforeQueries,
                "same-serial capacity wait neither allocates nor rescans");
        if (mode != "lifecycle-no-demand") {
            ++g_serial;
            for (unsigned i = 1400; i < 1432; ++i) Require(GetProgram(3,i).ok, "bounded32 fresh demand programs use reserved headroom");
            DemandPixels(1400); Require(WarmPending() == before, "demand never retires optional FIFO units");
        }
        for (unsigned round = 0; round < 200 && WarmPending(); ++round) {
            const unsigned beforeDelete = deletes, beforePending = WarmPending();
            g_serial += 600; vgx::CmdBuf empty; Execute(empty); CheckFifo(total);
            Require(WarmPending() < beforePending || deletes > beforeDelete,
                    "eligible quiet Execute retires queue work or restores consumed reserve without fresh demand");
            if (mode == "lifecycle-no-demand" || utilities)
                Require(deletes - beforeDelete <= 4, "new pressure reclamation deletes at most four names per Execute");
        }
        Require(WarmPending() == 0, "aged optional FIFO drains without unrelated new demand or external eviction");
        Require(rejected == 0, "successful bounded sequence never exhausts native table");
    }
    Require(glGetError() == GL_NO_ERROR, "GLES remains valid");
    char output[512]; const int n = std::snprintf(output,sizeof output,
        "PASS mode=%s creates=%u deletes=%u rejected=%u live=%zu pending=%u queries=%u physical_vita=false\n",
        mode.c_str(),creates,deletes,rejected,livePrograms.size(),WarmPending(),queries);
    Require(n>0 && size_t(n)<sizeof output && ::write(STDOUT_FILENO,output,n)==n,"write result");
}
