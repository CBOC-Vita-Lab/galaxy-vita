// Exercise actual backend eviction with Mesa; the 1024 slot limit is injected.
#define main quiet_fixture_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "vgx_backend_quiet.cpp"
#pragma clang diagnostic pop
#undef main
#include <unordered_set>

namespace {
std::unordered_set<GLuint> livePrograms;
unsigned creates = 0, deletes = 0, rejected = 0, peak = 0;
void Prepare(unsigned count) {
    const char* vertex = "attribute vec4 a_pos; void main(){gl_Position=a_pos;}";
    const char* fragment = "precision mediump float; void main(){gl_FragColor=vec4(0.25,0.75,0.5,1.0);}";
    const GLuint vs = Shader(GL_VERTEX_SHADER,vertex), fs = Shader(GL_FRAGMENT_SHADER,fragment);
    static vgx::ShaderSource vertexSource, fragmentSource;
    vertexSource.text = vertex; fragmentSource.text = fragment; fragmentSource.fragment = true;
    using namespace vgx::backend;
    g_vs.resize(4); g_vs[3] = vs; g_vsSrc.resize(4); g_vsSrc[3] = &vertexSource;
    g_fs.assign(count,fs); g_fsSrc.assign(count,&fragmentSource);
}
}
extern "C" GLuint __real_glCreateProgram();
extern "C" GLuint __wrap_glCreateProgram() {
    if (livePrograms.size() >= 1024) { ++rejected; return ~0u; }
    const GLuint id = __real_glCreateProgram(); Require(id != 0,"real Mesa allocates program");
    livePrograms.insert(id); ++creates; peak = std::max(peak,unsigned(livePrograms.size())); return id;
}
extern "C" void __real_glDeleteProgram(GLuint);
extern "C" void __wrap_glDeleteProgram(GLuint id) {
    Require(id && id != ~0u,"invalid program must never enter eviction");
    if (livePrograms.erase(id)) ++deletes;
    __real_glDeleteProgram(id);
}
int main(int argc,char** argv) {
    Require(argc == 2,"choose relink, capacity, used-pressure or burst");
    const std::string mode = argv[1];
    setenv("GXV_MAXPROGRAMS",mode == "relink" ? "150" : "900",1);
    Context(); Require(vgx::backend::Init(32,32),"real backend initialized");
    Prepare(mode == "relink" ? 151 : 1280);
    using namespace vgx::backend;
    unsigned linked = 0, evictionFrames = 0;
    if (mode == "relink") {
        g_serial = 500;
        for (unsigned id=0; id<150; ++id) {
            auto& p = GetProgram(3,id); Require(p.ok,"old program linked"); NoteVariant(p,3,id,0,0xc00,false);
        }
        g_serial = 1100;
        auto& fresh = GetProgram(3,150); Require(fresh.ok && fresh.usedSerial == 0,"new link is not counted as a draw");
        const auto id = fresh.id; const unsigned before = creates;
        g_serial = 1101; EvictPrograms();
        Require(g_programs.size() == 50 && deletes == 101,"existing eligibility and eviction batch size are preserved");
        Require(g_programs.count((3u<<16)|150u) == 1,"older eligible programs are evicted before a newly linked program");
        Require(GetProgram(3,150).id == id && creates == before,"later request reuses the fresh link instead of relinking");
        // A fresh link remains eligible: sorting must not accidentally protect it.
        g_serial = 1102;
        for (unsigned n=0; n<150; ++n) GetProgram(3,n);
        const unsigned oldDeletes = deletes; EvictPrograms();
        Require(deletes > oldDeletes,"never-drawn recent links remain eligible when there are no older candidates");
        linked = before; evictionFrames = 2;
    } else {
        Require(mode == "capacity" || mode == "used-pressure" || mode == "burst","known capacity scenario");
        if (mode == "burst") {
            vgx::CmdBuf commands; commands.frame = 1000; g_serial = 1000;
            for (unsigned id=0; id<1280; ++id) commands.cmds.push_back({vgx::CMD_PROGRAM,(3u<<16)|id});
            Execute(commands); linked = unsigned(g_programs.size());
            Require(rejected == 1 && linked == 1023,"one burst exhaustion is bounded without caching failed handles");
        } else for (unsigned frame=0; frame<40; ++frame) {
            g_serial = 1000 + frame; const auto oldDeletes = deletes; EvictPrograms(); evictionFrames += deletes != oldDeletes;
            for (unsigned n=0; n<32; ++n) {
                const unsigned id = frame * 32 + n; auto& p = GetProgram(3,id);
                if (p.ok) {
                    ++linked;
                    if (mode == "used-pressure") NoteVariant(p,3,id,0,0xc00,false);
                }
            }
        }
        if (mode == "capacity") Require(linked == 1280 && rejected == 0 && peak < 1024,"ordering-only change does not protect fresh links from capacity control");
        if (mode == "used-pressure") {
            Require(linked == 1023 && rejected == 9,"recently used capacity pressure retries at most once each affected frame");
            g_serial = 2000; const auto oldDeletes = deletes; EvictPrograms(); evictionFrames += deletes != oldDeletes;
            Require(GetProgram(3,1279).ok && rejected == 9,"a deferred program recovers after ordinary boundary eviction frees capacity");
            ++linked;
        }
    }
    Require(glGetError() == GL_NO_ERROR,"eviction and capacity model leave real GL valid");
    char result[512]; const int length = std::snprintf(result,sizeof result,
        "{\"mode\":\"%s\",\"linked\":%u,\"creates\":%u,\"deletes\":%u,\"eviction_frames\":%u,\"peak_live_programs\":%u,\"rejected\":%u,\"physical_vita\":false}\n",
        mode.c_str(),linked,creates,deletes,evictionFrames,peak,rejected);
    Require(length > 0 && size_t(length) < sizeof result,"bounded eviction report");
    Require(::write(STDOUT_FILENO,result,length) == length,"eviction report written");
}
