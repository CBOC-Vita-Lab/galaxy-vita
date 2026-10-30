// Real Mesa renders; only allocation failures and one link failure are injected.
#define main quiet_fixture_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "vgx_backend_quiet.cpp"
#pragma clang diagnostic pop
#undef main
#define GXV_RECIPE_SEED_BATCH 1
#include "../platform/vgx/src/vgx_recipe_seed.h"

namespace {
unsigned programCreates = 0, shaderCreates = 0, invalidHandles = 0, links = 0;
unsigned quadDraws = 0;
unsigned failProgram = 0, failShader = 0, failLinks = 0, failCompiles = 0;
GLuint failedHandle = 0;
bool failLink = false;
bool Invalid(GLuint id) { return id == 0 || id == ~0u; }
void RegisterPair(bool invalidSource = false, bool invalidLink = false, bool invalidFragment = false) {
    static vgx::ShaderSource vertex, fragment;
    vertex.id = 3; fragment.id = 4; fragment.fragment = true;
    vertex.keyHash = 0x1234; fragment.keyHash = 0x5678;
    vertex.text = invalidSource ? "not valid shader source" :
        "attribute vec4 a_pos; uniform mat4 u_proj; void main(){gl_Position=u_proj*a_pos;}";
    fragment.text = "precision mediump float; void main(){gl_FragColor=vec4(0.25,0.75,0.5,1.0);}";
    if (invalidFragment) fragment.text = "not valid fragment source";
    if (invalidLink) {
        vertex.text = "attribute vec4 a_pos; varying vec4 mismatch; void main(){gl_Position=a_pos;mismatch=a_pos;}";
        fragment.text = "precision mediump float; varying vec3 mismatch; void main(){gl_FragColor=vec4(mismatch,1.0);}";
    }
    vgx::backend::RegisterShader(&vertex); vgx::backend::RegisterShader(&fragment);
}
size_t AuditLines() {
    return std::count_if(fixture::summaryLines.begin(), fixture::summaryLines.end(),
        [](const std::string& s) { return s.find("[resource-audit]") == 0; });
}
bool AuditHas(const char* part) {
    return std::any_of(fixture::summaryLines.begin(), fixture::summaryLines.end(),
        [part](const std::string& s) { return s.find("[resource-audit]") == 0 && s.find(part) != std::string::npos; });
}
void RequireGreenPixels(bool copy = false) {
    const auto pixels = Read();
    const size_t center = (16 * 32 + 16) * 4;
    Require(pixels[center] == 64 && pixels[center + 1] == 191 && pixels[center + 2] == 128 && pixels[center + 3] == 255,
            copy ? "recovered utility program draws unchanged RGBA pixels" : "recovered game program draws unchanged RGBA pixels");
}
void RenderRecovered() {
    auto commands = Triangle(); commands.draws.resize(1); commands.cmds.clear();
    commands.draws[0].vsId = 3; commands.draws[0].fsId = 4;
    commands.cmds.push_back({vgx::CMD_DRAW, 0});
    vgx::backend::Execute(commands);
    RequireGreenPixels();
}
void RenderCopy() {
    using namespace vgx::backend;
    BindFbo(g_efbFbo); SetViewport(0,0,32,32); SetScissor(false); SetBlendOff(); SetDepthTest(false);
    SetColorMask(true,true);
    const float zero[16] = {}, color[4] = {0.25f,0.75f,0.5f,1};
    DrawQuad(g_whiteTex,0,0,1,1,zero,color); RequireGreenPixels(true);
}
}
extern "C" GLuint __real_glCreateProgram();
extern "C" GLuint __wrap_glCreateProgram() {
    ++programCreates;
    if (failProgram) { --failProgram; return failedHandle; }
    return __real_glCreateProgram();
}
extern "C" GLuint __real_glCreateShader(GLenum);
extern "C" GLuint __wrap_glCreateShader(GLenum type) {
    ++shaderCreates;
    if (failShader) { --failShader; return failedHandle; }
    return __real_glCreateShader(type);
}
extern "C" void __real_glAttachShader(GLuint,GLuint);
extern "C" void __wrap_glAttachShader(GLuint program,GLuint shader) {
    if (Invalid(program) || Invalid(shader)) ++invalidHandles;
    __real_glAttachShader(program,shader);
}
extern "C" void __real_glShaderSource(GLuint,GLsizei,const GLchar* const*,const GLint*);
extern "C" void __wrap_glShaderSource(GLuint shader,GLsizei count,const GLchar* const* parts,const GLint* lengths) {
    if (Invalid(shader)) ++invalidHandles;
    __real_glShaderSource(shader,count,parts,lengths);
}
extern "C" void __real_glCompileShader(GLuint);
extern "C" void __wrap_glCompileShader(GLuint shader) {
    if (Invalid(shader)) ++invalidHandles;
    if (failCompiles) { --failCompiles; return; }
    __real_glCompileShader(shader);
}
extern "C" void __real_glLinkProgram(GLuint);
extern "C" void __wrap_glLinkProgram(GLuint program) {
    ++links;
    if (Invalid(program)) ++invalidHandles;
    if (failLink) { failLink = false; return; }
    if (failLinks) { --failLinks; return; }
    __real_glLinkProgram(program);
}
extern "C" void __real_glDrawArrays(GLenum,GLint,GLsizei);
extern "C" void __wrap_glDrawArrays(GLenum mode,GLint first,GLsizei count) {
    ++quadDraws; __real_glDrawArrays(mode,first,count);
}
int main(int argc,char** argv) {
    Require(argc == 2,"choose one allocation recovery scenario");
    const std::string mode = argv[1];
    const char* auditEnv = std::getenv("GXV_RESOURCE_AUDIT");
    const bool audit = auditEnv && std::strcmp(auditEnv,"1") == 0;
    Context();
#if VGX_COLDTRACE
    Require(vgx::coldtrace::g_recorder.Arm(TraceClock,TraceThread,1),"recovery paths arm RAM tracing");
#endif
    if (mode.compare(0,5,"init-") == 0 || mode.compare(0,8,"utility-") == 0) {
        failProgram = mode == "init-program" || mode == "utility-program" || mode == "utility-max";
        failShader = mode == "init-shader" || mode == "utility-shader";
        failLink = mode == "init-link" || mode == "utility-link";
        failedHandle = mode == "utility-max" ? ~0u : 0;
        Require(!vgx::backend::Init(32,32),"backend must not report successful initialization without its essential copy program");
        Require(invalidHandles == 0,"failed utility initialization never uses an invalid handle");
        if (mode.compare(0,8,"utility-") == 0) {
            using namespace vgx::backend;
            Require(g_copyProg == 0,"failed utility creation is not published as usable");
            const auto failedPrograms = programCreates, failedShaders = shaderCreates;
            for (unsigned i=0; i<128; ++i) Require(!EnsureCopyProgram(),"utility failure backs off this frame");
            Require(programCreates == failedPrograms && shaderCreates == failedShaders,"utility failure does not cause a same-frame allocation storm");
            const auto drawsBefore = quadDraws;
            const float zero[16] = {}, color[4] = {};
            DrawQuad(g_whiteTex,0,0,1,1,zero,color);
            Require(quadDraws == drawsBefore && invalidHandles == 0,"unavailable utility program is not drawn");
            ++g_serial; Require(EnsureCopyProgram(),"utility retries successfully in a later frame");
            Require(shaderCreates == (mode == "utility-shader" ? 3u : 2u),"successful utility shader compiles are retained across retry");
            Require(programCreates == (mode == "utility-program" || mode == "utility-max" ? 2u : 1u),"utility retry retains its allocated program after link failure");
            RenderCopy();
        }
    } else {
        Require(vgx::backend::Init(32,32),"real backend initialized");
        using namespace vgx::backend;
        g_serial = 41;
        if (mode == "seed-sweep") {
            const char* path = std::getenv("RECOVERY_SEED_FILE");
            std::vector<uint8_t> bytes; vgx::recipe::List list;
            Require(path && vgx::recipe::Read(path,vgx::recipe::kMaxListSize,bytes) && vgx::recipe::Parse(bytes,list),
                    "caller-supplied shader recipe passes the production structural parser");
            std::vector<vgx::ShaderSource> vertex(list.vs.size()), fragment(list.fs.size());
            for (size_t i=0;i<vertex.size();++i) {
                vgx::GenerateVertexShader(list.vs[i],vertex[i]); vertex[i].id=i;
                vertex[i].keyHash=vgx::HashKey(&list.vs[i],sizeof(list.vs[i])); RegisterShader(&vertex[i]); CompileShader(false,i);
                Require(g_vs[i]!=0,"every recipe vertex key compiles through the actual backend and Mesa");
            }
            for (size_t i=0;i<fragment.size();++i) {
                vgx::GenerateFragmentShader(list.fs[i],fragment[i]); fragment[i].id=i; fragment[i].fragment=true;
                fragment[i].keyHash=vgx::HashKey(&list.fs[i],sizeof(list.fs[i])); RegisterShader(&fragment[i]); CompileShader(true,i);
                Require(g_fs[i]!=0,"every recipe fragment key compiles through the actual backend and Mesa");
            }
            unsigned pairs=0;
            for (const auto& record:list.records) if (record.type==3) {
                Require(GetProgram(record.v[0],record.v[1]).ok,"every recorded recipe pair links through the actual backend and Mesa"); ++pairs;
            }
            char report[160]; const int n=std::snprintf(report,sizeof report,"seed sweep: vertex=%zu fragment=%zu pairs=%u Mesa-only; no native patcher or route coverage claim\n",vertex.size(),fragment.size(),pairs);
            ::write(2,report,n);
        } else if (mode == "source-error") {
            RegisterPair(true); const auto firstLinks = links, firstShaders = shaderCreates;
            Require(!GetProgram(3,4).ok,"invalid source is rejected");
            ++g_serial;
            for (unsigned i=0; i<128; ++i) Require(!GetProgram(3,4).ok,"source error stays rejected");
            Require(links == firstLinks && shaderCreates == firstShaders + 2,
                    "a rejected source never links and neither stage recompiles before the retry delay");
        } else if (mode == "compile-exhaustion" || mode == "fragment-compile-exhaustion" || mode == "link-exhaustion" || mode == "injected-link-exhaustion") {
            const bool fragmentFailure = mode == "fragment-compile-exhaustion";
            const bool compile = mode == "compile-exhaustion" || fragmentFailure;
            RegisterPair(compile && !fragmentFailure, mode == "link-exhaustion", fragmentFailure);
            if (mode == "injected-link-exhaustion") failLinks = 4;
            const auto firstLinks = links, firstShaders = shaderCreates;
            const uint64_t serials[] = {41,101,221,461};
            for (unsigned n=0;n<4;++n) {
                g_serial = serials[n]; g_progFrame = 1000+n;
                Require(!GetProgram(3,4).ok,"each deliberately failed compile/link remains unavailable");
                Require(compile ? (fragmentFailure ? g_fsAttempts[4] : g_vsAttempts[3]).failures == n+1 : g_programs.at((3u<<16)|4u).failures == n+1,
                        "four-attempt counter advances exactly once at each scheduled retry");
                const auto shaders = shaderCreates, programs = programCreates;
                for (unsigned i=0;i<128;++i) Require(!GetProgram(3,4).ok,"failed program stays unavailable in this serial");
                ++g_serial; Require(!GetProgram(3,4).ok,"one serial later is still in delayed backoff");
                Require(shaderCreates == shaders && programCreates == programs,"delayed retry never reallocates early");
            }
            g_serial = 100000;
            auto commands = Triangle(); commands.draws.resize(1); commands.cmds={{vgx::CMD_DRAW,0}};
            commands.draws[0].vsId=3; commands.draws[0].fsId=4;
            const auto draws = actualDraws; Execute(commands);
            Require(actualDraws == draws,"exhausted compile/link omits the real draw even much later");
            Require(links == firstLinks + (compile ? 0 : 4) && shaderCreates == firstShaders + (compile ? 5 : 2),
                    "exact GL call counts establish four failed attempts, with successful stages retained");
            Require(!audit || (AuditHas("disposition=exhausted") && AuditHas("stage=draw-skip") &&
                    AuditHas("frame=1003") && AuditHas("attempt=4") && AuditHas("0000000000001234")),
                    "quiet basic log retains exhaustion, frame, attempt, hash and omitted-draw evidence");
        } else if (mode == "link-recovered" || mode == "compile-recovered") {
            RegisterPair(); failLinks=mode == "link-recovered"; failCompiles=mode == "compile-recovered"; g_progFrame=2000;
            Require(!GetProgram(3,4).ok,"injected false compile/link status defers the program");
            g_serial+=60; Require(GetProgram(3,4).ok,"later successful link recovers without changing policy");
            RenderRecovered();
            Require(!audit || AuditHas("disposition=recovered"),"a recorded failure has a later success record");
        } else if (mode == "declared-white") {
            RegisterPair(); Require(GetProgram(3,4).ok,"texture fallback fixture links");
            auto commands = Triangle(); commands.draws.resize(1); commands.cmds={{vgx::CMD_DRAW,0}};
            auto& draw=commands.draws[0]; draw.vsId=3; draw.fsId=4;
            const auto before=AuditLines(); Execute(commands);
            Require(AuditLines()==before,"ordinary untextured draw does not report missing textures");
            draw.texMask=1; draw.tex[0].texId=0; Execute(commands);
            Require(!audit || AuditHas("reason=zero-id"),"declared zero ID is reported in the quiet basic log");
            draw.tex[0].texId=65000; Execute(commands);
            g_tex.resize(10); draw.tex[0].texId=9; Execute(commands);
            Require(!audit || (AuditHas("reason=out-of-range") && AuditHas("reason=no-gl-name") && AuditHas("slot=0")),
                    "out-of-range and absent GL names are distinguished for declared slots");
            const auto once=AuditLines();
            for (unsigned i=0;i<512;++i) Execute(commands);
            Require(!audit || AuditLines()<once+20,"repeated fallback records are deduplicated with bounded summaries");
            for (unsigned i=0;i<1024;++i) { draw.tex[0].texId=1000+i; Execute(commands); }
            Require(!audit || (AuditLines()<200 && AuditHas("suppressed=")),"unique fallback floods are capped with a suppression summary");
            RequireGreenPixels();
        } else if (mode == "audit-read-once") {
            RegisterPair(true); Require(!GetProgram(3,4).ok,"invalid source exercises first audit read");
            setenv("GXV_RESOURCE_AUDIT",audit ? "0" : "1",1);
            g_serial+=60; Require(!GetProgram(3,4).ok,"invalid source exercises another audit read");
            Require(audit ? AuditHas("attempt=2") : AuditLines()==0,"audit switch is read once independently of diagnostic build flags");
        } else if (mode == "missing-source") {
            Require(!GetProgram(3,4).ok,"unregistered source cannot be linked");
            Require(!audit || AuditHas("stage=missing-source"),"unregistered sources are identified without a bogus compiler error");
            RegisterPair();
            Require(GetProgram(3,4).ok,"later shader registration is not poisoned by a missing-source cache entry");
            RenderRecovered();
        } else if (mode == "sentinel" || mode == "frame-retry" || mode == "fragment-failure") {
            RegisterPair();
            if (mode == "fragment-failure") {
                CompileShader(false,3); const auto beforeShaders = shaderCreates;
                failShader = 1; Require(!GetProgram(3,4).ok,"fragment allocation fails after vertex compilation");
                Require(g_fsPending[4] && !g_vsPending[3],"only failed fragment stays pending");
                ++g_serial; Require(GetProgram(3,4).ok,"fragment allocation recovers");
                Require(shaderCreates == beforeShaders + 2,"retry compiles only the missing fragment, not the vertex again");
            } else if (mode == "sentinel") {
                failProgram = 1; auto& failed = GetProgram(3,4); Require(!failed.ok,"resource failure returns unavailable program");
                failed.id = 123; failed.ok = true;
                const auto& next = GetProgram(3,5);
                Require(!next.ok && next.id == 0,"unavailable sentinel cannot inherit caller mutations");
                ++g_serial; Require(GetProgram(3,4).ok,"mutated sentinel never enters the program cache");
            } else {
                failProgram = 3; const auto before = programCreates;
                for (unsigned frame=0; frame<3; ++frame) {
                    for (unsigned i=0; i<128; ++i) Require(!GetProgram(3,4).ok,"persistent failure remains deferred");
                    Require(programCreates == before + frame + 1,"persistent resource failure attempts allocation once per frame");
                    ++g_serial;
                }
                Require(GetProgram(3,4).ok,"three failed frames do not permanently poison later recovery");
            }
            RenderRecovered();
        } else if (mode == "warm-demand-program" || mode == "warm-demand-shader" || mode == "warm-persistent") {
            RegisterPair(); auto commands = Triangle(); commands.draws.resize(1); commands.cmds.clear();
            commands.draws[0].vsId = 3; commands.draws[0].fsId = 4;
            for (unsigned i=0; i<257; ++i) commands.cmds.push_back({vgx::CMD_DRAW,0});
            commands.warm.push_back({vgx::CMD_PROGRAM,(3u<<16)|4u}); SetWarmBudget(1000);
            const auto beforePrograms = programCreates, beforeShaders = shaderCreates;
            const unsigned beforeDraws = actualDraws;
            failProgram = mode == "warm-persistent" ? 6 : (mode == "warm-demand-program");
            failShader = mode == "warm-demand-shader";
            if (mode == "warm-persistent") {
                for (unsigned frame=0; frame<3; ++frame) {
                    const auto serial = g_serial; Execute(commands); commands.warm.clear();
                    Require(g_serial == serial + 1,"Execute advances the allocation budget serial exactly once");
                    Require(programCreates == beforePrograms + (frame + 1) * 2 && actualDraws == beforeDraws,
                            "persistent exhaustion is bounded to one warm and one demand failure per Execute");
                    Require(WarmPending() == 2 && g_warm.size() == 1,"persistent failure does not lose queued work");
                }
                Require(failProgram == 0,"all six bounded injected failures were exercised");
                Execute(commands);
                Require(actualDraws == beforeDraws + 257 && WarmPending() == 0,"fourth Execute recovers actual draws and retained warm work");
            } else {
                const auto serial = g_serial; Execute(commands); commands.warm.clear();
                Require(g_serial == serial + 1,"Execute advances the allocation budget serial exactly once");
                Require(actualDraws == beforeDraws + 257,"a transient speculative warm failure must not veto demand draws in the same Execute");
                Require(failProgram == 0 && failShader == 0,"warm failure injection was consumed");
                Require(programCreates == beforePrograms + (mode == "warm-demand-program" ? 2u : 1u) &&
                        shaderCreates == beforeShaders + (mode == "warm-demand-shader" ? 3u : 2u),"exact allocation counts prove warm failure followed by demand retry");
                const auto issuedPrograms = programCreates, issuedShaders = shaderCreates;
                const auto sameSerial = g_serial;
                RunWarm();
                Require(WarmPending() == 2 && g_warm.size() == 1 && g_serial == sameSerial &&
                        programCreates == issuedPrograms && shaderCreates == issuedShaders,"failed warm phase remains deferred even after demand caches its result");
                Execute(commands);
                Require(WarmPending() == 0 && programCreates == issuedPrograms && shaderCreates == issuedShaders,
                        "next Execute retires warm work using the demand cache without recompiling");
            }
            RequireGreenPixels();
        } else if (mode == "demand-first") {
            RegisterPair(); failProgram = 1;
            Require(!GetProgram(3,4).ok,"direct demand can set resource backoff");
            vgx::CmdBuf queued; DeferWarm(queued,{vgx::CMD_PROGRAM,(3u<<16)|4u}); SetWarmBudget(1000);
            const auto before = programCreates; RunWarm();
            Require(GlAllocBackoff() && programCreates == before && WarmPending() == 2,"warm entry cannot clear a demand failure or attempt allocation");
            Require(!GetProgram(3,5).ok && programCreates == before,"warm entry cannot re-enable other same-frame misses after demand failure");
            ++g_serial; RunWarm(); Require(WarmPending() == 0 && GetProgram(3,4).ok,"demand-first failure recovers at a later serial");
            RenderRecovered();
        } else if (mode == "warm-program" || mode == "warm-shader") {
            RegisterPair(); vgx::CmdBuf queued;
            if (mode == "warm-program") {
                DeferWarm(queued,{vgx::CMD_PROGRAM,(3u<<16)|4u});
                DeferWarm(queued,{vgx::CMD_PROGRAM,(3u<<16)|4u});
                failProgram = 1;
            } else {
                queued.shaders.push_back(g_vsSrc[3]);
                DeferWarm(queued,{vgx::CMD_SHADER,0}); failShader = 1;
            }
            const unsigned units = WarmPending(); const auto items = g_warm.size();
            SetWarmBudget(1000); RunWarm();
            Require(WarmPending() == units && g_warm.size() == items,"resource failure preserves deferred warm work for retry");
            const auto failedPrograms = programCreates, failedShaders = shaderCreates;
            RunWarm();
            Require(programCreates == failedPrograms && shaderCreates == failedShaders,"warm slices back off rather than spin during resource failure");
            ++g_serial; RunWarm();
            Require(WarmPending() == 0 && g_warm.empty(),"deferred work resumes after the allocation backoff");
            Require(GetProgram(3,4).ok,"deferred recovery yields a valid program"); RenderRecovered();
        } else {
            Require(mode == "good" || mode == "program-zero" || mode == "program-max" || mode == "shader-zero" || mode == "shader-max",
                    "known program recovery scenario");
            ::Program(1,0); Require(GetProgram(1,0).ok,"prime cached program before failure"); RegisterPair();
            failedHandle = mode.find("max") != std::string::npos ? ~0u : 0;
            failProgram = mode.compare(0,7,"program") == 0;
            failShader = mode.compare(0,6,"shader") == 0;
            if (mode != "good") {
                const auto beforePrograms = programCreates, beforeShaders = shaderCreates;
                const auto oldProgramStats = g_stats.programs;
                Require(!GetProgram(3,4).ok,"injected allocation failure cannot produce a usable program");
                Require(invalidHandles == 0,"failed handles never reach attach, source, compile or link");
                Require(!g_programs.count((3u<<16)|4u),"transient allocation failure is not permanently cached");
                Require(g_stats.programs == oldProgramStats,"failed allocation is not counted as a program");
                const auto failedPrograms = programCreates, failedShaders = shaderCreates;
                for (unsigned i=0; i<128; ++i) {
                    Require(!GetProgram(3,4).ok,"failed key stays deferred for this frame");
                    Require(GetProgram(1,0).ok,"resource failure does not suppress cached programs");
                    GetProgram(3,5); // a different miss must not restart allocation in this frame
                    CompileShader(false,3);
                }
                Require(programCreates == failedPrograms && shaderCreates == failedShaders,"allocation retries are bounded across keys and direct shader requests");
                Require(mode.compare(0,6,"shader") != 0 || (g_vsPending[3] && g_vs[3] == 0),"shader allocation failure keeps source pending");
                ++g_serial;
                Require(GetProgram(3,4).ok,"allocation retries successfully in a later frame");
                Require(programCreates > beforePrograms && shaderCreates > beforeShaders,"real allocation resumes after transient failure");
            } else Require(GetProgram(3,4).ok,"ordinary game program links successfully");
            RenderRecovered();
        }
        Require(invalidHandles == 0 && glGetError() == GL_NO_ERROR,"recovery leaves a valid real GL pipeline");
    }
    Require(invalidHandles == 0 && glGetError() == GL_NO_ERROR,"all recovery scenarios leave real GL valid");
    Require(audit || AuditLines()==0,"audit off emits no resource log records");
#if VGX_COLDTRACE
    vgx::coldtrace::g_recorder.RequestFreeze();
    const auto view = vgx::coldtrace::g_recorder.Frozen();
    Require(view.data && view.data->worstCount > 0 && !view.clockErrors,"allocation failure returns close RAM trace scopes cleanly");
#endif
    for (const auto& line : fixture::summaryLines)
        if (line.find("[resource-audit]") == 0 && line.find("disposition=exhausted") != std::string::npos) {
            ::write(2,line.data(),line.size()); ::write(2,"\n",1);
        }
    char report[512];
    const int length = std::snprintf(report,sizeof report,"{\"mode\":\"%s\",\"trace\":%d,\"audit\":%d,\"audit_lines\":%zu,\"passed\":true,\"program_creates\":%u,\"shader_creates\":%u,\"links\":%u,\"draws\":%u,\"invalid_handle_calls\":%u,\"physical_vita\":false}\n",
                                   mode.c_str(),VGX_COLDTRACE,audit,AuditLines(),programCreates,shaderCreates,links,actualDraws,invalidHandles);
    Require(length > 0 && size_t(length) < sizeof report,"bounded recovery test result");
    Require(::write(STDOUT_FILENO,report,length) == length,"recovery result written");
}
