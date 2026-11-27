#define main quiet_fixture_main
#include "vgx_backend_quiet.cpp"
#undef main
#include <set>
namespace reference {
#include "vgx_present_reference.inc"
}
namespace fallback {
std::string mode;
unsigned vertexCompiles=0,halfCompiles=0,floatCompiles=0,links=0;
std::set<GLuint> shaders,programs;
}
extern "C" void __real_glCompileShader(GLuint);
extern "C" void __wrap_glCompileShader(GLuint shader) {
    using namespace fallback;
    shaders.insert(shader);GLint kind=0;glGetShaderiv(shader,GL_SHADER_TYPE,&kind);
    char source[8192];glGetShaderSource(shader,sizeof source,nullptr,source);
    const bool half=std::strstr(source,"#define RGBV half3")!=nullptr;
    if(kind==GL_VERTEX_SHADER)++vertexCompiles;
    else {
        if(half)++halfCompiles;else {++floatCompiles;Require(std::string(source)==std::string("#version 100\n")+reference::fs,"fallback retains exact original five-tap float source");}
    }
    const bool fail=kind==GL_VERTEX_SHADER?mode=="vertex":mode=="both-compile"||(mode=="half-compile"&&half);
    if(fail){const char* invalid="not a shader";glShaderSource(shader,1,&invalid,nullptr);}
    __real_glCompileShader(shader);
}
extern "C" void __real_glDeleteShader(GLuint);
extern "C" void __wrap_glDeleteShader(GLuint shader) {
    Require(fallback::shaders.erase(shader)==1,"shader released once");__real_glDeleteShader(shader);
}
extern "C" GLuint __real_glCreateProgram();
extern "C" GLuint __wrap_glCreateProgram(){GLuint p=__real_glCreateProgram();fallback::programs.insert(p);return p;}
extern "C" void __real_glDeleteProgram(GLuint);
extern "C" void __wrap_glDeleteProgram(GLuint p){Require(fallback::programs.erase(p)==1,"program released once");__real_glDeleteProgram(p);}
extern "C" void __real_glLinkProgram(GLuint);
extern "C" void __wrap_glLinkProgram(GLuint p){++fallback::links;__real_glLinkProgram(p);}
extern "C" void __real_glGetProgramiv(GLuint,GLenum,GLint*);
extern "C" void __wrap_glGetProgramiv(GLuint p,GLenum name,GLint* value) {
    __real_glGetProgramiv(p,name,value);
    if(name==GL_LINK_STATUS&&(fallback::mode=="both-link"||(fallback::mode=="half-link"&&fallback::links==1)))*value=GL_FALSE;
}
int main(int argc,char** argv) {
    Require(argc==2,"fallback scenario required");fallback::mode=argv[1];Context();
    using namespace vgx::backend;using namespace fallback;
    const bool good=mode=="success"||mode=="half-compile"||mode=="half-link";
    Require(EnsureSharpenProgram()==good,"half failure falls back once to original float");
    Require(vertexCompiles==1,"vertex compiles only once");
    Require(halfCompiles==(mode=="vertex"?0u:1u),"half compiles at most once");
    Require(floatCompiles==((mode=="success"||mode=="vertex")?0u:1u),"original float fallback is bounded to one attempt");
    Require(shaders.empty()&&programs.size()==(good?1u:0u),"all temporary ownership released");
    Require(g_sharp.failed==!good,"permanent failure only after bounded fallback");
    const auto before=halfCompiles+floatCompiles+vertexCompiles+links;
    Require(EnsureSharpenProgram()==good,"cached success or failure stable");
    Require(before==halfCompiles+floatCompiles+vertexCompiles+links,"no repeated retry loop");
    if(good)Require(g_sharp.rect>=0&&g_sharp.size>=0&&g_sharp.lim>=0&&g_sharp.p>=0,"four uniforms remain active");
    std::printf("PASS fallback=%s vertex=%u half=%u original=%u links=%u program_live=%zu\n",mode.c_str(),vertexCompiles,halfCompiles,floatCompiles,links,programs.size());
}
