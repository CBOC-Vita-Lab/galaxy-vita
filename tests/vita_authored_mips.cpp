// Real cache -> real backend -> extracted release driver -> exact storage -> Mesa LOD pixels.
#define main original_quiet_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "vgx_backend_quiet.cpp"
#pragma clang diagnostic pop
#undef main
#include "vgx_state.h"
#include "vgx_texdecode.h"
namespace vgx { void SubmitToBackend(CmdBuf& b) { backend::Execute(b); } }
extern "C" {
void fixture_image(int,int,int,const void*);
void fixture_subimage(int,int,int,const void*);
void fixture_generate(void);
void fixture_reset(void);
void fixture_mark_used(void);
void fixture_pixels(unsigned,unsigned,unsigned,void*);
unsigned fixture_mips(void);
unsigned fixture_descriptor_mips(void);
unsigned fixture_waits(void);
unsigned fixture_transfers(void);
unsigned fixture_allocations(void);
int sceGxmTransferFinish(void);
void vgxvita_uploadAuthoredI4Mips(unsigned,unsigned,unsigned,const void* const*) __attribute__((weak));
void __real_glTexImage2D(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
}
#include "authored-host.inc"

extern "C" void __wrap_glTexImage2D(GLenum t,GLint l,GLint internal,GLsizei w,GLsizei h,GLint border,GLenum f,GLenum type,const void* p) {
    Require(t==GL_TEXTURE_2D && internal==GL_RGBA && !border && f==GL_RGBA && type==GL_UNSIGNED_BYTE,"native I4 path stays lossless RGBA8");
    fixture_image(l,w,h,p);
}
extern "C" void __wrap_glTexSubImage2D(GLenum t,GLint l,GLint x,GLint y,GLsizei w,GLsizei h,GLenum f,GLenum type,const void* p) {
    Require(t==GL_TEXTURE_2D && !x && !y && f==GL_RGBA && type==GL_UNSIGNED_BYTE,"native subimage covers one authored RGBA level");
    fixture_subimage(l,w,h,p);
}
extern "C" void __wrap_glGenerateMipmap(GLenum t) { Require(t==GL_TEXTURE_2D,"generate target"); fixture_generate(); sceGxmTransferFinish(); }

namespace {
const unsigned values[]={0,17,170,238,255,51,102,153,204,85,119,136,187};
void Patch(uint64_t,unsigned w,unsigned h,uint8_t* p) { std::memset(p,91,w*h*4); }
bool RejectPatch(uint64_t,unsigned,unsigned) { return false; }
bool Replace(const vgx::TexReplaceQuery& q,vgx::TexUpload& up) {
    up.width=q.width;up.height=q.height;up.levels.emplace_back(size_t(up.width)*up.height*4,77);return true;
}
void Controls() {
    for(unsigned kind=0;kind<14;++kind) {
        vgx::ResetState();vgx::g.bpTexStale=0;vgx::Pending().clear();
        auto& slot=vgx::g.tex[0];slot.valid=true;slot.obj.width=16;slot.obj.height=kind==0?8:kind==1?9:16;
        slot.obj.format=kind>=2&&kind<=8?kind-1:kind==9?0xE:0;
        slot.obj.mipmap=1;slot.obj.maxLod=3;slot.obj.minFilter=kind==10?vgx::FILT_LINEAR:vgx::FILT_LIN_MIP_LIN;
        if(kind==11)slot.obj.maxLod=0;
        auto bytes=std::vector<uint8_t>(vgx::TexDataSize(slot.obj.width,slot.obj.height,slot.obj.format,4),0x11);
        slot.obj.data=bytes.data();slot.avail=kind==12?vgx::TexLevelSize(16,16,0):bytes.size();
        if(kind==13)vgx::SetTexturePatchHook(Patch);
        vgx::TexBind bind;uint16_t w=0,h=0;Require(vgx::ResolveTexture(0,bind,w,h),"excluded texture remains resolvable");
        Require(!vgx::Pending().uploads.back().preserveI4Mips,"rectangular/NPOT/non-I4/nonmip/incomplete/patched textures keep old path");
        vgx::SetTexturePatchHook(nullptr);
    }
    for(bool replacement:{false,true}) {
        vgx::ResetState();vgx::g.bpTexStale=0;vgx::Pending().clear();
        auto bytes=std::vector<uint8_t>(vgx::TexDataSize(16,16,0,4),0x11);auto& slot=vgx::g.tex[0];
        slot.valid=true;slot.obj.data=bytes.data();slot.obj.width=slot.obj.height=16;slot.obj.format=0;slot.obj.mipmap=1;
        slot.obj.maxLod=3;slot.obj.minFilter=vgx::FILT_LIN_MIP_LIN;slot.avail=bytes.size();
        vgx::SetTexturePatchHook(Patch,RejectPatch);
        if(replacement)vgx::SetTextureReplaceHook(Replace,vgx::FILT_LIN_MIP_LIN);
        vgx::TexBind bind;uint16_t w=0,h=0;Require(vgx::ResolveTexture(0,bind,w,h),"HD/declined patch candidate resolves");
        Require(vgx::Pending().uploads.back().preserveI4Mips==!replacement,"HD remains independent; declined color patch allows original authored I4");
        vgx::SetTexturePatchHook(nullptr);vgx::SetTextureReplaceHook(nullptr,vgx::FILT_LIN_MIP_LIN);
    }
}
void PixelOracle(const std::vector<std::vector<uint8_t>>& levels,unsigned width) {
    GLuint tex=0;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
    for(unsigned l=0;l<levels.size();++l) {
        const unsigned w=width>>l;
        __real_glTexImage2D(GL_TEXTURE_2D,l,GL_RGBA,w,w,0,GL_RGBA,GL_UNSIGNED_BYTE,levels[l].data());
    }
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,levels.size()-1);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    static GLuint program=0;
    if(!program) {
        const auto vs=Shader(GL_VERTEX_SHADER,"#version 300 es\nvoid main(){vec2 p=vec2(gl_VertexID==1?3.0:-1.0,gl_VertexID==2?3.0:-1.0);gl_Position=vec4(p,0,1);}");
        const auto fs=Shader(GL_FRAGMENT_SHADER,"#version 300 es\nprecision highp float;uniform highp sampler2D tex;uniform float lod;out vec4 color;void main(){color=textureLod(tex,vec2(0.5),lod);}");
        program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);GLint ok=0;glGetProgramiv(program,GL_LINK_STATUS,&ok);Require(ok,"LOD oracle shader links");
    }
    glUseProgram(program);glUniform1i(glGetUniformLocation(program,"tex"),0);glViewport(0,0,32,32);
    for(unsigned half=0;half<2*levels.size()+1;++half) {
        const float lod=half*0.5f;glUniform1f(glGetUniformLocation(program,"lod"),lod);glDrawArrays(GL_TRIANGLES,0,3);
        const unsigned low=std::min(half/2,unsigned(levels.size()-1)),high=std::min(low+1,unsigned(levels.size()-1));
        const float expected=half%2?(values[low]+values[high])*0.5f:values[low];
        const auto pixels=Read();for(uint8_t p:pixels)Require(std::abs(p-expected)<=1.0f,"all RGBA pixels preserve authored integer and fractional LOD values and terminal clamp");
    }
    glDeleteTextures(1,&tex);vgx::backend::S={};
}
}

int main(int argc,char** argv) {
    Require(argc==2,"usage: authored-mips on|off|default-off|ordinary-loss|controls|pattern");
    const std::string mode=argv[1];
    Context();
    if(mode=="controls") {
        Require(vgx::backend::GetCaps().authoredI4Mips,"controls require opt-in native capability");
        setenv("GXV_AUTHORED_I4_MIPS","0",1);Require(vgx::backend::GetCaps().authoredI4Mips,"capability is read once");Controls();
        const char message[]="PASS: 16 exclusion/HD/patch cases and read-once gate\n";::write(1,message,sizeof message-1);return 0;
    }
    if(mode=="pattern") {
        Require(vgxvita_uploadAuthoredI4Mips,"authored helper exists");
        for(unsigned size:{2u,4u,8u,16u,128u,2048u}) {
            fixture_reset();const unsigned count=32u-__builtin_clz(size);std::vector<std::vector<uint8_t>> expected;const void* ptrs[13];
            for(unsigned l=0;l<count;++l) {
                const unsigned w=size>>l;expected.emplace_back(w*w*4);
                for(unsigned y=0;y<w;++y)for(unsigned x=0;x<w;++x)for(unsigned c=0;c<4;++c)expected[l][(y*w+x)*4+c]=((x*3+y*7+l*5)%16)*17;
                ptrs[l]=expected[l].data();
            }
            for(unsigned repeat=0;repeat<3;++repeat) {
                if(repeat)fixture_mark_used();
                vgxvita_uploadAuthoredI4Mips(size,size,count,ptrs);
                for(unsigned l=0;l<count;++l) {auto actual=expected[l];fixture_pixels(l,size>>l,size>>l,actual.data());Require(actual==expected[l],"every patterned texel survives row padding, mip offsets and same-name redefinition");}
            }
        }
        fixture_reset();const char message[]="PASS: 18 patterned full-chain uploads, padded mips, CPU/transfer generation and redefinitions\n";::write(1,message,sizeof message-1);return 0;
    }
    if(mode=="ordinary-loss") {
        fixture_reset();unsigned w=128;
        for(unsigned l=0;l<5;++l,w/=2) { std::vector<uint8_t> px(w*w*4,values[l]);fixture_image(l,w,w,px.data());sceGxmTransferFinish(); }
        for(unsigned l=1;l<5;++l) { std::vector<uint8_t> px((128>>l)*(128>>l)*4);fixture_pixels(l,128>>l,128>>l,px.data());for(auto p:px)Require(p==0,"ordinary supplied levels really regenerate from zero base"); }
        const char message[]="PASS: pinned driver ordinary higher glTexImage2D loses authored opacity even with mipGen=0\n";::write(1,message,sizeof message-1);fixture_reset();return 0;
    }
    const bool on=mode=="on";
    Require(on||mode=="off"||mode=="default-off","known pipeline mode");
    Require(vgx::backend::GetCaps().authoredI4Mips==on,"default/off/on capability selection");
    setenv("GXV_AUTHORED_I4_MIPS",on?"0":"1",1);
    Require(vgx::backend::GetCaps().authoredI4Mips==on,"on and off both remain fixed after first capability read");
    for(unsigned size:{2u,4u,8u,16u,128u,256u}) for(unsigned count=2;count<=32u-__builtin_clz(size);++count) {
        fixture_reset();std::vector<uint8_t> gx;
        for(unsigned l=0;l<count;++l)gx.insert(gx.end(),vgx::TexLevelSize(size>>l,size>>l,0),uint8_t(values[l]));
        vgx::ResetState();vgx::g.bpTexStale=0;vgx::Pending().clear();auto& slot=vgx::g.tex[0];
        slot.valid=true;slot.obj.data=gx.data();slot.obj.width=slot.obj.height=size;slot.obj.format=0;slot.obj.mipmap=1;slot.obj.maxLod=count-1;slot.obj.minFilter=vgx::FILT_LIN_MIP_LIN;slot.obj.magFilter=1;slot.avail=gx.size();
        vgx::TexBind binding;uint16_t w=0,h=0;Require(vgx::ResolveTexture(0,binding,w,h),"real I4 cache resolves");
        const auto& up=vgx::Pending().uploads.back();
        Require(up.levels.size()==(on?count:1u)&&up.genMips==!on,"opt-in carries all authored levels; default keeps generation policy");
        const auto beforeUploads=vgx::backend::C.texUploads,beforeBytes=vgx::backend::C.texUploadBytes;
        vgx::backend::Upload(up);
        size_t payload=0;for(const auto& level:up.levels)payload+=level.size();
        Require(vgx::backend::C.texUploads-beforeUploads==up.levels.size() && vgx::backend::C.texUploadBytes-beforeBytes==payload,"payload counters include every authored level without counting scratch allocation as data");
        // The pinned driver's glGenerateMipmap exposes log2(size) levels, not its allocated 1x1 tail.
        const auto actual=fixture_mips();Require(actual==(on?count:31u-__builtin_clz(size)),"native allocation keeps requested chain length or unchanged generation baseline");
        Require(fixture_descriptor_mips()==actual,"actual GXM descriptor matches native mip count");
        Require(fixture_waits()==1 && fixture_allocations()==1,"one allocation growth and mandatory transfer wait per mip chain");
        std::vector<std::vector<uint8_t>> pixels;
        for(unsigned l=0;l<(on?count:1u);++l) {
            const unsigned lw=size>>l;pixels.emplace_back(lw*lw*4);fixture_pixels(l,lw,lw,pixels.back().data());
            for(auto p:pixels.back())Require(p==values[l],"every driver mip byte retains authored gradual alpha");
        }
        if(on)PixelOracle(pixels,size);
        const auto id=binding.texId;Require(vgx::ResolveTexture(0,binding,w,h)&&binding.texId==id&&vgx::Pending().uploads.size()==1,"cache reuse leaves authored allocation unchanged");
        auto& t=vgx::backend::TexSlotFor(up.texId);glDeleteTextures(1,&t.id);t={};vgx::backend::S={};
    }
    fixture_reset();const char message[]="PASS: real cache/backend/pinned driver storage and Mesa LOD pixels; not native GPU proof\n";::write(1,message,sizeof message-1);
}
