#define main quiet_fixture_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#define VGX_TEST_NATIVE_CAPACITY
#include "vgx_backend_quiet.cpp"
#pragma clang diagnostic pop
#undef main
#include <unordered_set>
#ifdef BASELINE_BUILD
// Compile-only compatibility for branches never run by the baseline comparison.
namespace vgx::backend { bool g_nativeCapacityPressure=false; bool WarmCapacityYielded(){return false;} }
#endif

namespace {
std::array<GLuint,1024> nativeSlots{};
unsigned creates=0, deletes=0, rejects=0, queries=0;
GLuint injectedName=0;
bool injectFailure=false;
uint64_t slotVisits=0;
unsigned quiet=1;
std::vector<GLuint> utilities;
unsigned Live() {
    return unsigned(std::count_if(nativeSlots.begin(),nativeSlots.end(),[](GLuint id){return id!=0;}));
}
void Emit(const char* label) {
    char out[512];
    const int n=std::snprintf(out,sizeof out,
        "%s creates=%u deletes=%u rejected=%u live=%u pending=%u queries=%u slot_visits=%llu serial=%llu physical_vita=false\n",
        label,creates,deletes,rejects,Live(),vgx::backend::WarmPending(),queries,
        static_cast<unsigned long long>(slotVisits),static_cast<unsigned long long>(vgx::backend::g_serial));
    Require(n>0 && size_t(n)<sizeof out && ::write(STDOUT_FILENO,out,n)==n,"write counters");
}
void Prepare() {
    static vgx::ShaderSource vs,fs;
    vs.id=3; fs.fragment=true;
    vs.text="attribute vec4 a_pos; void main(){gl_Position=a_pos;}";
    fs.text="precision mediump float; void main(){gl_FragColor=vec4(0.25,0.75,0.5,1.0);}";
    using namespace vgx::backend;
    g_vs.resize(4); g_vs[3]=Shader(GL_VERTEX_SHADER,vs.text.c_str());
    g_vsSrc.resize(4); g_vsSrc[3]=&vs;
    g_fs.assign(1600,Shader(GL_FRAGMENT_SHADER,fs.text.c_str())); g_fsSrc.assign(1600,&fs);
}
void Queue(unsigned fs) {
    vgx::CmdBuf b; b.variants.push_back({3,static_cast<uint16_t>(fs),0,0xc00});
    vgx::backend::DeferWarm(b,{vgx::CMD_VARIANT,0});
}
void FillUtilities(unsigned live) {
    while(Live()<live) utilities.push_back(glCreateProgram());
}
void DemandPixels(unsigned fs) {
    using namespace vgx::backend;
    auto b=Triangle(); b.draws[0].vsId=3; b.draws[0].fsId=static_cast<uint16_t>(fs);
    UploadStream(b); Draw(b,b.draws[0]); const auto p=Read(); const size_t c=(16*32+16)*4;
    Require(p[c]==64 && p[c+1]==191 && p[c+2]==128 && p[c+3]==255,"fresh program renders exact RGBA");
}
void AgeExecute() {
    vgx::backend::g_serial+=600; vgx::CmdBuf b; vgx::backend::Execute(b);
}
}
extern "C" uint32_t port_scene_key(){return 7;}
extern "C" int port_screen_quiet(){return quiet;}
extern "C" int port_vgx_vita_splash_active(){return 0;}
extern "C" unsigned vglGetFreeProgramSlots() {
    ++queries; unsigned free=0;
    // Match the native query's slot traversal, not an O(1) occupancy substitute.
    for(GLuint id:nativeSlots){++slotVisits; if(!id)++free;}
    return free;
}
extern "C" GLuint __real_glCreateProgram();
extern "C" GLuint __wrap_glCreateProgram() {
    if(injectFailure){injectFailure=false;++rejects;return injectedName;}
    const auto slot=std::find(nativeSlots.begin(),nativeSlots.end(),0u);
    if(slot==nativeSlots.end()){++rejects;return 0;}
    const GLuint id=__real_glCreateProgram(); Require(id && id!=~0u,"valid real program name");
    *slot=id; ++creates; return id;
}
extern "C" void __real_glDeleteProgram(GLuint);
extern "C" void __wrap_glDeleteProgram(GLuint id) {
    const auto slot=std::find(nativeSlots.begin(),nativeSlots.end(),id);
    Require(id && id!=~0u && slot!=nativeSlots.end(),"delete owns a live name");
    *slot=0; ++deletes; __real_glDeleteProgram(id);
}
int main(int argc,char**argv) {
    Require(argc==2,"one adversarial scenario"); const std::string mode=argv[1];
    const bool off=mode=="off",negative=mode=="negative";
    setenv("GXV_MAXPROGRAMS",off?"0":negative?"-1":"900",1);
    setenv("GXV_GL_RETRY","1",1);
    Context(); Require(vgx::backend::Init(32,32),"real backend initialized"); Prepare();
    using namespace vgx::backend;
    g_serial=1000; SetWarmBudget(1000000);
    if(mode=="resident-head") {
        FillUtilities(900); Queue(0); RunWarm();
        Require(WarmPending()==1 && WarmCapacityYielded(),"missing head yields on actual capacity");
        const auto before=creates;
        Require(GetProgram(3,0).ok,"same-serial demand makes head resident");
        Require(creates==before+1,"demand uses one reserved slot");
        RunWarm(); Emit("resident-head");
        Require(WarmPending()==0 && !WarmCapacityYielded(),"resident head retires in same serial without capacity gate");
    } else if(mode=="utility-only") {
        FillUtilities(1024); Queue(0); RunWarm();
        Require(WarmPending()==1 && WarmCapacityYielded() && rejects==0,"utility-only full table yields before allocation");
        const auto before=deletes; AgeExecute();
        Require(deletes==before && g_programs.empty() && WarmPending()==1,"pressure never steals unmanaged utility names");
        for(unsigned i=0;i<WarmProgramReserve()+1;++i)glDeleteProgram(utilities[i]);
        vgx::CmdBuf b; Execute(b); Emit("utility-only");
        Require(WarmPending()==0 && rejects==0,"real returned utility capacity permits next-frame progress");
        DemandPixels(0);
    } else if(mode=="retry-future" || mode=="retry-eligible") {
        FillUtilities(900);
        vgx::backend::Program failed; failed.failures=2; failed.retryAfter=mode=="retry-future"?2000:999;
        const uint32_t key=3u<<16; g_programs.emplace(key,failed); Queue(0);
        const auto before=creates; RunWarm(); Emit(mode.c_str());
        const auto& record=g_programs.at(key);
        Require(!record.id && record.failures==2 && record.retryAfter==failed.retryAfter,"failed id0 retains exact retry history under pressure");
        Require(creates==before && g_programRetryFrame!=g_serial,"refused or not-yet-due retry consumes no attempt budget");
        if(mode=="retry-future") {
            Require(WarmPending()==0 && queries==0,"not-yet-due failure is not mistaken for missing capacity work");
        } else {
            Require(WarmPending()==1 && WarmCapacityYielded(),"eligible id0 retry is capacity guarded");
            Require(GetProgram(3,0).ok && g_programRetryFrame==g_serial,"demand may spend one eligible retry");
            failed.retryAfter=999; g_programs.emplace(key|1,failed);
            const auto after=creates; Require(!GetProgram(3,1).ok && creates==after,"second same-frame retry stays bounded");
        }
    } else if(mode=="front-uaf") {
        for(unsigned i=0;i<12;++i)Require(GetProgram(3,i).ok,"seed resident programs");
        FillUtilities(900); Queue(100); RunWarm();
        const GLuint old=g_programs.at(3u<<16).id;
        Require(GetProgram(3,0).id==old,"front holds victim before retirement");
        AgeExecute();
        Require(g_programs.find(3u<<16)==g_programs.end(),"known old victim retired");
        Require(GetProgram(3,0).ok,"front miss safely reacquires evicted pair");
        DemandPixels(0); Emit("front-uaf");
    } else if(mode=="failed-history-eviction") {
        for(unsigned i=0;i<901;++i)Require(GetProgram(3,i).ok,"seed ordinary-eviction pressure");
        for(unsigned i=1200;i<1210;++i) {
            vgx::backend::Program failed; failed.failures=kGlMaxAttempts; failed.retryAfter=999;
            g_programs.emplace((3u<<16)|i,failed);
        }
        AgeExecute(); Emit("failed-history-eviction");
        for(unsigned i=1200;i<1210;++i) {
            const auto found=g_programs.find((3u<<16)|i);
            Require(found!=g_programs.end() && !found->second.id && found->second.failures==kGlMaxAttempts && found->second.retryAfter==999,
                    "ordinary retirement cannot erase exhausted id0 retry history");
        }
    } else if(mode=="failure-free" || mode=="failure-uintmax" || mode=="failure-full") {
        if(mode=="failure-full")FillUtilities(1024);
        else {injectFailure=true;injectedName=mode=="failure-uintmax"?~0u:0;}
        Require(!GetProgram(3,0).ok && rejects==1 && g_programs.empty(),"invalid allocation creates no resident or failed-link metadata");
        Emit(mode.c_str());
        Require(g_nativeCapacityPressure==(mode=="failure-full"),"only authoritative free0 latches native capacity pressure");
        Require(GlAllocBackoff(),"allocation failure keeps its ordinary same-serial backoff");
    } else if(mode=="shader-tail") {
        FillUtilities(900); Queue(0); RunWarm();
        static vgx::ShaderSource tail; tail.id=1599; tail.fragment=true;
        tail.text="precision mediump float; void main(){gl_FragColor=vec4(0.25,0.75,0.5,1.0);}";
        vgx::CmdBuf b; b.shaders.push_back(&tail); DeferWarm(b,{vgx::CMD_SHADER,0});
        Require(WarmPending()==11 && g_fsPending[1599],"blocked head retains shader's ten weighted units");
        Require(GetProgram(3,1599).ok && !g_fsPending[1599],"demand compiles registered shader behind capacity-blocked FIFO");
        Require(WarmPending()==11,"demand leaves optional queue accounting unchanged");
        DemandPixels(1599); Emit("shader-tail");
    } else if(mode=="recent-link") {
        for(unsigned i=0;i<898;++i)Require(GetProgram(3,i).ok,"seed old eligible residents");
        g_serial=1700; Require(GetProgram(3,1000).ok,"fresh linked resident without draw");
        FillUtilities(900); Queue(1100); RunWarm();
        const auto id=g_programs.at((3u<<16)|1000).id;
        vgx::CmdBuf b; Execute(b); Emit("recent-link");
        Require(g_programs.at((3u<<16)|1000).id==id,"pressure preserves recent link despite no actual use");
    } else if(mode=="steady-cost") {
        Require(GetProgram(3,0).ok,"seed demand cache");
        for(unsigned i=0;i<1024;++i)Require(GetProgram(3,0).ok,"cached demand remains resident");
        quiet=0; Queue(1);
        for(unsigned i=0;i<100;++i){vgx::CmdBuf b;Execute(b);}
        Emit("steady-cost");
        Require(queries==0 && WarmPending()==1,"cached demand and active deferred workload add no native query scans");
    } else if(mode=="ordinary-budget-reference") {
        for(unsigned i=0;i<901;++i)Require(GetProgram(3,i).ok,"seed identical baseline ordinary map pressure");
        const auto before=deletes; AgeExecute(); Emit("ordinary-budget-reference");
        Require(deletes-before==101,"historical ordinary eviction deletes101 from901 toward800");
    } else if(mode=="overlap-additional") {
        for(unsigned i=0;i<901;++i)Require(GetProgram(3,i).ok,"seed ordinary pressure with full native utilities");
        FillUtilities(1024); Require(!GetProgram(3,1200).ok,"full native table records demand failure");
        const auto before=deletes; AgeExecute(); Emit("overlap-additional");
#ifdef BASELINE_BUILD
        Require(deletes-before==101,"baseline full-table ordinary eviction deletes101");
#else
        Require(deletes-before>101 && deletes-before<=105,"new pressure adds at most four names to the legacy101-delete batch");
#endif
    } else if(mode=="overlap-budget") {
        for(unsigned i=0;i<901;++i)Require(GetProgram(3,i).ok,"seed map above historical target");
        Queue(1000); RunWarm();
        const auto before=deletes; AgeExecute(); Emit("overlap-budget");
        Require(deletes-before<=4,"ordinary plus pressure deletion stays within requested four-name Execute budget");
    } else if(off || negative) {
        for(unsigned i=0;i<12;++i)Require(GetProgram(3,i).ok,"seed aging programs");
        FillUtilities(900); Queue(100); RunWarm();
        const auto before=deletes; AgeExecute(); Emit(mode.c_str());
        Require(deletes==before,"previous no-reclamation setting semantics unchanged");
    } else if(mode=="work-cost") {
        for(unsigned i=0;i<1280;++i)Queue(i);
        RunWarm(); Emit("work-cost-initial");
        const auto beforeQueries=queries,beforeCreates=creates;
        for(unsigned i=0;i<64;++i)WarmIdle();
        Require(queries==beforeQueries && creates==beforeCreates,"blocked same-serial polling does not repeat native query");
        while(WarmPending())AgeExecute();
        Emit("work-cost-drained");
    } else Require(false,"known scenario");
    Require(glGetError()==GL_NO_ERROR,"real GL pipeline remains valid");
    Emit("PASS");
}
