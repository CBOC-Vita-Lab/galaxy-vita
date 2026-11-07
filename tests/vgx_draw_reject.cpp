// Actual backend + existing quiet Mesa fixture. No game process or target device.
#define main quiet_fixture_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "vgx_backend_quiet.cpp"
#pragma clang diagnostic pop
#undef main
namespace { bool failNextProgram=false; }
extern "C" GLuint __real_glCreateProgram();
extern "C" GLuint __wrap_glCreateProgram() {
    if(failNextProgram) {failNextProgram=false;return 0;}
    return __real_glCreateProgram();
}
int main(int argc,char** argv) {
    Require(argc==2,"one case");const std::string mode=argv[1];
    if(mode=="sanitize") setenv("GXV_SANITIZE","3",1);
    Context();using namespace vgx;using namespace vgx::backend;
    Require(Init(32,32),"backend initializes");
    auto b=Triangle();b.draws.resize(1);b.cmds.clear();b.frame=123;
    Execute(b); // Uploads/registration are explicit below, not inferred from a draw.
    static ShaderSource vs,fs;vs.id=3;fs.id=4;fs.fragment=true;
    vs.text="attribute vec4 a_pos;uniform mat4 u_proj;void main(){gl_Position=u_proj*a_pos;}";
    fs.text="precision mediump float;void main(){gl_FragColor=vec4(0.25,0.75,0.5,1.0);}";
    if(mode=="compile") vs.text="invalid vertex source";
    if(mode=="link") {
        vs.text="attribute vec4 a_pos;varying vec4 mismatch;void main(){gl_Position=a_pos;mismatch=a_pos;}";
        fs.text="precision mediump float;varying vec3 mismatch;void main(){gl_FragColor=vec4(mismatch,1.0);}";
    }
    if(mode!="missing") {RegisterShader(&vs);RegisterShader(&fs);}
    b.draws[0].vsId=3;b.draws[0].fsId=4;
    auto& r=b.draws[0];
    if(mode=="static") r.vbuf=UINT32_MAX;
    if(mode=="stream") r.vchunk=UINT16_MAX;
    if(mode=="state") r.rs.scW=0;
    if(mode=="sanitize") r.idxOffset=UINT32_MAX;
    if(mode=="skip") g_skipDraws=true;
    if(mode=="empty") r.idxCount=0;
    if(mode=="allocation") failNextProgram=true;
    const auto draws=actualDraws;const auto lines=fixture::summaryLines.size();
    Draw(b,r);
#if GXV_DRAW_REJECT_COUNTERS
    const auto beforeReport=g_drawReject;
    Require(fixture::summaryLines.size()==lines,"no per-draw printing");
    if(mode=="empty") Require(beforeReport.nonempty==0 && beforeReport.rejected==0,"empty draw excluded");
    else if(mode=="good") Require(beforeReport.nonempty==1 && beforeReport.submitted==1 && !beforeReport.rejected,"submitted draw denominator");
    else {
        Require(beforeReport.nonempty==1,"nonempty denominator");
        Require(beforeReport.rejected==1,"one exclusive rejection");
        if(mode=="allocation") Require(beforeReport.programState[0]==1,"allocation backoff observed");
        if(mode=="compile") Require(beforeReport.programState[1]==1,"compile failure observed");
        if(mode=="link") Require(beforeReport.programState[2]==1,"link failure observed");
        if(mode=="missing") Require(beforeReport.programState[3]==1,"missing source observed");
        const unsigned reason=mode=="static"?2:mode=="stream"?3:mode=="state"?5:mode=="sanitize"?4:mode=="skip"?0:1;
        Require(beforeReport.reason[reason]==1 && beforeReport.first[reason].frame==123,"exact reason and frame");
    }
    DrawRejectReport();
    Require(g_drawReject.nonempty==0 && g_drawReject.rejected==0,"report resets window once");
    Require(fixture::summaryLines.size()>lines,"bounded report emitted at explicit cadence hook");
#endif
    Require(actualDraws==draws+(mode=="good"?1u:0u),"counter does not change GL draw disposition");
    if(mode=="good") {
        const auto pixels=Read();const size_t center=(16*32+16)*4;
        Require(pixels[center]==64 && pixels[center+1]==191 && pixels[center+2]==128 && pixels[center+3]==255,"same exact green RGBA with macro on/off");
    }
    if(mode=="allocation") {
        ++g_serial;Require(GetProgram(3,4).ok,"same renderer recovers next serial");
    }
#if GXV_DRAW_REJECT_COUNTERS
    if(mode=="skip") {
        g_drawReject.nonempty=UINT64_MAX;g_drawReject.reason[RejectSkip]=UINT64_MAX;
        Draw(b,r);Require(g_drawReject.overflow && g_drawReject.nonempty==UINT64_MAX && g_drawReject.reason[RejectSkip]==UINT64_MAX,"bounded saturating counters");
    }
#endif
    return 0;
}
