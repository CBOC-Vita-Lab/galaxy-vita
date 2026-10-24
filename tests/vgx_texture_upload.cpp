// Actual backend and Mesa; exact frozen vitaGL allocator, with SDK/heap doubles.
#define main quiet_fixture_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "vgx_backend_quiet.cpp"
#pragma clang diagnostic pop
#undef main
#include <memory>
#include "vgx_frozen_texture.h"

namespace {
struct Block { std::unique_ptr<uint8_t[]> bytes; size_t size; bool live=true, temp=false; };
struct Pending { void* dst; const void* src; size_t size; };
struct TransferHazard {};
struct Metrics { size_t grows=0, moved=0, fallback=0, peak=0, transfers=0, waits=0; };
std::vector<Block> blocks;
std::vector<Pending> pending;
Metrics metrics;
texture nativeTexture{};
bool deferredFree=false, capture=false, nativeUpload=false, allowHazard=false;
#ifdef __VGX_VITA__
constexpr bool nativeOrdering=true;
#else
constexpr bool nativeOrdering=false;
#endif
std::string heapMode;
struct Call { int level,w,h,bytes; bool compressed; const void* data; };
std::vector<Call> calls;
unsigned mipGens=0;
bool skipGeneration=false;
size_t LiveBytes() { size_t n=0; for(const auto& b:blocks) if(b.live&&!b.temp)n+=b.size; return n; }
Block* Range(const void* p,size_t n) {
    const auto a=reinterpret_cast<uintptr_t>(p);
    for(auto& b:blocks) {
        const auto lo=reinterpret_cast<uintptr_t>(b.bytes.get());
        if(b.live&&a>=lo&&a-lo<=b.size&&n<=b.size-(a-lo)) return &b;
    }
    return nullptr;
}
bool Overlap(const void* a,size_t an,const void* b,size_t bn) {
    const auto x=reinterpret_cast<uintptr_t>(a),y=reinterpret_cast<uintptr_t>(b);
    return x<y+bn&&y<x+an;
}
void Safe(const void* p,size_t n) {
    for(const auto& q:pending) if(Overlap(p,n,q.dst,q.size)||Overlap(p,n,q.src,q.size)) {
        Require(allowHazard,"pending transfer lifetime hazard");
        throw TransferHazard{};
    }
}
void* Allocate(size_t size,bool temp=false) {
    // Reserve backing space for deterministic in-place growth; track logical bounds separately.
    auto p=std::make_unique<uint8_t[]>(2*1024*1024);
    std::memset(p.get(),0xcd,2*1024*1024); void* result=p.get();
    blocks.push_back({std::move(p),size,true,temp}); metrics.peak=std::max(metrics.peak,LiveBytes()); return result;
}
void Retire(void* p) {
    if(!p)return; Block* b=Range(p,1); Require(b&&b->bytes.get()==p,"retire a live allocation"); Safe(p,b->size);
    if(!deferredFree)b->live=false;
}
void Reset(const std::string& mode,bool defer) {
    Require(pending.empty(),"all transfers completed before resetting fixture");
    blocks.clear(); metrics={}; nativeTexture={}; nativeTexture.use_mips=true;
    calls.clear(); mipGens=0; heapMode=mode; deferredFree=defer;
}
vgx::TexUpload Chain(unsigned w,unsigned h,uint8_t format=vgx::TF_DXT1,bool full=false) {
    vgx::TexUpload up; up.texId=71;up.width=w;up.height=h;up.format=format;
    const uint16_t colors[]={0xf800,0x07e0,0x001f,0xffff,0xffe0,0xf81f,0x07ff,0x8410,0x4208};
    unsigned l=0;
    for(;;++l) {
        if(format==vgx::TF_DXT1) {
            up.levels.emplace_back(((w+3)/4)*((h+3)/4)*8,0);
            auto& bytes=up.levels.back(); const uint16_t c=colors[l%9];
            for(size_t j=0;j<bytes.size();j+=8){bytes[j]=c&255;bytes[j+1]=c>>8;}
        } else up.levels.emplace_back(w*h*(format==vgx::TF_LA88?2:4),uint8_t(l*29+17));
        if(full?(w==1&&h==1):(w/2<4||h/2<4))break;
        w=w>1?w/2:1;h=h>1?h/2:1;
    }
    up.authored=up.levels.size(); return up;
}
void SelectTexture(GLuint id) { glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,id); }
void SampleMips(GLuint id,const vgx::TexUpload& up) {
    static GLuint program=0;
    if(!program) {
        const GLuint vs=Shader(GL_VERTEX_SHADER,
            "#version 300 es\nvoid main(){vec2 p=vec2((gl_VertexID==1)?3.0:-1.0,(gl_VertexID==2)?3.0:-1.0);gl_Position=vec4(p,0,1);}");
        const GLuint fs=Shader(GL_FRAGMENT_SHADER,
            "#version 300 es\nprecision highp float;uniform highp sampler2D tex;uniform float lod;out vec4 color;void main(){color=textureLod(tex,vec2(0.5),lod);}");
        program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);
        GLint ok=0;glGetProgramiv(program,GL_LINK_STATUS,&ok);Require(ok,"explicit LOD sampling program linked");
    }
    SelectTexture(id);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,up.levels.size()-1);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST_MIPMAP_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,32,32);glDisable(GL_DEPTH_TEST);glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);
    glColorMask(true,true,true,true);glUseProgram(program);glUniform1i(glGetUniformLocation(program,"tex"),0);
    for(size_t l=0;l<up.levels.size();++l) {
        glUniform1f(glGetUniformLocation(program,"lod"),l);glDrawArrays(GL_TRIANGLES,0,3);
        const auto pixels=Read();
        uint8_t expected[4];
        if(up.format==vgx::TF_DXT1) {
            const auto c=unsigned(up.levels[l][0])|(unsigned(up.levels[l][1])<<8);
            const unsigned r=(c>>11)&31,g=(c>>5)&63,b=c&31;
            expected[0]=(r<<3)|(r>>2);expected[1]=(g<<2)|(g>>4);expected[2]=(b<<3)|(b>>2);expected[3]=255;
        } else if(up.format==vgx::TF_LA88) {
            expected[0]=expected[1]=expected[2]=up.levels[l][0];expected[3]=up.levels[l][1];
        } else std::memcpy(expected,up.levels[l].data(),4);
        for(size_t p=0;p<pixels.size();++p) Require(std::abs(int(pixels[p])-int(expected[p%4]))<=1,"every authored mip samples its own unchanged color");
    }
}
struct Result { Metrics metrics; std::vector<uint8_t> bytes; std::vector<Call> calls; unsigned mipCount; };
Result Run(const vgx::TexUpload& up,bool backend,const std::string& mode,bool defer,bool redefine) {
    Reset(mode,defer); nativeUpload=true; capture=true;
    GLuint id=0;
    if(redefine) {
        auto old=Chain(1024,1024); glGenTextures(1,&id);SelectTexture(id);
        unsigned w=old.width,h=old.height;
        for(size_t l=0;l<old.levels.size();++l){glCompressedTexImage2D(GL_TEXTURE_2D,l,GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,w,h,0,old.levels[l].size(),old.levels[l].data());w/=2;h/=2;}
        calls.clear(); metrics={}; metrics.peak=LiveBytes();
    }
    const auto beforeBytes=vgx::backend::g_stats.textureBytes;
    const auto beforeUploads=vgx::backend::C.texUploads,beforeCountBytes=vgx::backend::C.texUploadBytes;
    if(backend) {
        auto& slot=vgx::backend::TexSlotFor(71);
        if(id)slot.id=id;
        vgx::backend::S.tex[0]=~0u;vgx::backend::S.unit=-1;
        vgx::backend::Upload(up);id=slot.id;
        Require(slot.w==up.width&&slot.h==up.height&&slot.mips==(up.levels.size()>1),"backend publishes unchanged texture metadata");
    } else {
        if(!id)glGenTextures(1,&id);SelectTexture(id);unsigned w=up.width,h=up.height;
        for(size_t l=0;l<up.levels.size();++l){glCompressedTexImage2D(GL_TEXTURE_2D,l,GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,w,h,0,up.levels[l].size(),up.levels[l].data());w=w>1?w/2:1;h=h>1?h/2:1;}
    }
    if(backend) {
        size_t n=0;for(const auto& level:up.levels)n+=level.size();
        Require(vgx::backend::g_stats.textureBytes-beforeBytes==n&&vgx::backend::C.texUploadBytes-beforeCountBytes==n&&
            vgx::backend::C.texUploads-beforeUploads==up.levels.size(),"backend upload byte and call accounting unchanged");
    }
    Require(pending.empty(),"host wrapper finishes each native compressed upload");
    Require(metrics.transfers==up.levels.size()&&metrics.waits==up.levels.size(),"same native transfer count and per-call completion waits");
    Require(calls.size()==up.levels.size()&&calls.front().level==0,"base first and every level uploaded once");
    unsigned w=up.width,h=up.height;size_t total=0;
    for(size_t l=0;l<up.levels.size();++l) {
        unsigned seen=0;for(const auto& c:calls)if(c.level==int(l)){++seen;Require(c.w==int(w)&&c.h==int(h)&&c.bytes==int(up.levels[l].size())&&c.data==up.levels[l].data(),"each level retains dimensions, bytes and data pointer");}
        Require(seen==1,"level is neither duplicated nor skipped");
        const int offset=test_gpu_mip_offset(l,up.width,up.height),size=test_gpu_mip_size(l,w,h);
        Require(offset==int(total)&&test_gpu_chain_size(l,up.width,up.height)==offset+size,"real frozen mip offsets and chain sizes agree");
        Require(!std::memcmp(static_cast<uint8_t*>(nativeTexture.data)+offset,up.levels[l].data(),size),"all allocator level regions retain their bytes");
        total+=size;w=w>1?w/2:1;h=h>1?h/2:1;
    }
    Require(nativeTexture.mip_count==int(up.levels.size())&&nativeTexture.gxm_tex.mips==up.levels.size()&&
            nativeTexture.gxm_tex.width==up.width&&nativeTexture.gxm_tex.height==up.height&&
            nativeTexture.gxm_tex.format==SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR,"frozen driver publishes unchanged chain descriptor");
    capture=false;nativeUpload=false;SampleMips(id,up);
    Result result{metrics,std::vector<uint8_t>(static_cast<uint8_t*>(nativeTexture.data),static_cast<uint8_t*>(nativeTexture.data)+total),calls,unsigned(nativeTexture.mip_count)};
    glDeleteTextures(1,&id);vgx::backend::TexSlotFor(71)={};vgx::backend::S={};return result;
}
void Controls() {
    using namespace vgx::backend;
    for(unsigned kind=0;kind<8;++kind) {
        Reset("move",false);nativeUpload=false;capture=true;
        auto up=Chain(kind==1?7:64,kind==1?5:64,kind==1?vgx::TF_LA88:kind==0||kind==3?vgx::TF_RGBA8:vgx::TF_DXT1);
        if(kind==6)up=Chain(60,28);
        if(kind==7)up=Chain(16,16,vgx::TF_DXT1,true);
        g_hasS3tc=kind!=2;
        up.genMips=kind==3||kind==4;
        // DXT1+genMips is not emitted by the frontend. Record its exclusion guard,
        // but do not ask GLES to generate an unsupported compressed destination.
        skipGeneration=kind==4;
        if(kind==3)up.levels.resize(1);
        if(kind==5){up.levels.resize(1);up.width=up.height=64;}
        S={};const auto beforeUploads=C.texUploads;Upload(up);const auto id=TexSlotFor(71).id;
        Require(calls.size()==up.levels.size()&&C.texUploads-beforeUploads==up.levels.size(),"control keeps all uploads");
        Require(mipGens==unsigned(up.genMips),"generated mip control preserves generation");
        unsigned w=up.width,h=up.height;
        for(size_t l=0;l<calls.size();++l) {
            Require(calls[l].level==int(l)&&calls[l].w==int(w)&&calls[l].h==int(h),"RGBA/LA88/software DXT/generated/short chains retain ascending order and dimensions");
            Require(calls[l].compressed==(up.format==vgx::TF_DXT1&&g_hasS3tc),"control preserves compressed versus uncompressed dispatch");
            w=w>1?w/2:1;h=h>1?h/2:1;
        }
        capture=false;skipGeneration=false;
        if(!up.genMips)SampleMips(id,up);
        const auto error=glGetError();
        char why[128];std::snprintf(why,sizeof why,"control GL upload remains valid (case %u, error 0x%x)",kind,error);
        Require(error==GL_NO_ERROR,why);
        glDeleteTextures(1,&id);TexSlotFor(71)={};S={};
    }
    g_hasS3tc=true;
}
Pixels SampleLinear(GLuint id, bool linear) {
    static GLuint program=0;
    if (!program) {
        const auto vs=Shader(GL_VERTEX_SHADER,
            "#version 300 es\nvoid main(){vec2 p=vec2((gl_VertexID==1)?3.0:-1.0,(gl_VertexID==2)?3.0:-1.0);gl_Position=vec4(p,0,1);}");
        const auto fs=Shader(GL_FRAGMENT_SHADER,
            "#version 300 es\nprecision highp float;uniform sampler2D tex;out vec4 color;void main(){color=texture(tex,gl_FragCoord.xy/24.0-vec2(0.16));}");
        program=glCreateProgram(); glAttachShader(program,vs); glAttachShader(program,fs); glLinkProgram(program);
        GLint ok=0; glGetProgramiv(program,GL_LINK_STATUS,&ok); Require(ok,"filtered compact texture shader links");
        glDeleteShader(vs); glDeleteShader(fs);
    }
    SelectTexture(id);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,linear?GL_LINEAR:GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,linear?GL_LINEAR:GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER,0); glViewport(0,0,32,32); glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glColorMask(true,true,true,true);
    glUseProgram(program); glUniform1i(glGetUniformLocation(program,"tex"),0);
    glDrawArrays(GL_TRIANGLES,0,3);
    return Read();
}
void CompactPixels() {
    using namespace vgx::backend;
    const unsigned dimensions[][2]={{1,1},{3,5},{7,5},{8,8},{9,9},{16,8},{127,65}};
    for (const auto& dim:dimensions) for (const bool linear:{false,true}) {
        vgx::TexUpload rgba; rgba.texId=71; rgba.width=dim[0]; rgba.height=dim[1];
        rgba.levels.emplace_back(dim[0]*dim[1]*4);
        auto la=rgba; la.format=vgx::TF_LA88; la.levels[0].resize(dim[0]*dim[1]*2);
        for (unsigned y=0;y<dim[1];++y) for (unsigned x=0;x<dim[0];++x) {
            const unsigned pixel=y*dim[0]+x;
            const uint8_t l=(x*29+y*13+17)&255,a=(x*7+y*31+91)&255;
            la.levels[0][pixel*2]=l; la.levels[0][pixel*2+1]=a;
            rgba.levels[0][pixel*4]=rgba.levels[0][pixel*4+1]=rgba.levels[0][pixel*4+2]=l;
            rgba.levels[0][pixel*4+3]=a;
        }
        S={}; Upload(rgba); const auto id=TexSlotFor(71).id;
        const auto reference=SampleLinear(id,linear);
        Upload(la);
        Require(TexSlotFor(71).id==id && !TexSlotFor(71).mips,"format replacement keeps the existing texture identity and base-level metadata");
        const auto compact=SampleLinear(id,linear);
        Require(reference==compact,"LA88 and RGBA have byte-identical nearest/linear pixels including odd-width edge clamp");
        rgba.levels[0][0]=237; rgba.levels[0][1]=81; rgba.levels[0][2]=46;
        Upload(rgba); const auto patched=SampleLinear(id,linear);
        Require(patched!=reference,"LA88 to colored RGBA replacement reaches actual sampling");
        Upload(la); Require(SampleLinear(id,linear)==reference,"colored RGBA to LA88 replacement restores exact original pixels");
        glDeleteTextures(1,&id); TexSlotFor(71)={}; S={};
    }
}
}

extern "C" void* gpu_alloc_mapped_for_gpu(size_t n){return Allocate(n);}
extern "C" void* gpu_alloc_mapped_temp(size_t n){return Allocate(n,true);}
extern "C" void* vgl_realloc(void* p,size_t n){
    ++metrics.grows;auto* b=Range(p,1);Require(b&&b->bytes.get()==p,"realloc starts at live allocation");
    if(heapMode=="inplace"){b->size=n;metrics.peak=std::max(metrics.peak,LiveBytes());return p;}
    if(heapMode=="fallback")return nullptr;
    const auto old=b->size;Safe(p,old);void* q=Allocate(n);std::memcpy(q,p,old);metrics.moved+=old;Range(p,1)->live=false;return q;
}
extern "C" void gpu_free_texture_data(texture* t){Retire(t->data);t->data=nullptr;t->palette_data=nullptr;}
extern "C" void vgl_memcpy(void* d,const void* s,size_t n){Safe(s,n);Require(Range(d,n)&&Range(s,n),"fallback copy stays within live ranges");std::memcpy(d,s,n);metrics.fallback+=n;}
extern "C" void vgl_fast_memcpy(void* d,const void* s,size_t n){Require(Range(d,n),"temporary upload copy within allocation");std::memcpy(d,s,n);}
extern "C" void vgl_memset(void* d,int c,size_t n){Require(Range(d,n),"zero fill within allocation");std::memset(d,c,n);}
extern "C" void vglGetTexSizes(SceGxmTexture* t,uint32_t* w,uint32_t* h){*w=t->width;*h=t->height;}
extern "C" void vglInitSwizzledTexture(SceGxmTexture* t,const void* p,SceGxmTextureFormat f,unsigned w,unsigned h,unsigned m){*t={w,h,m,f,p};}
extern "C" uint8_t tex_format_to_alignment(SceGxmTextureFormat){std::abort();}
extern "C" void dxt_compress(void*,uint8_t*,unsigned,unsigned,int){std::abort();}
#define UNUSED_SWIZZLE(name) extern "C" void name(uint8_t*,uint8_t*,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned){std::abort();}
UNUSED_SWIZZLE(SwizzleTexData64Bpp)
UNUSED_SWIZZLE(SwizzleTexData128Bpp)
UNUSED_SWIZZLE(SwizzleTexDataETC1)
extern "C" int sceGxmTransferCopy(unsigned w,unsigned h,unsigned,unsigned,int,int fmt,int,void* s,unsigned,unsigned,unsigned,
                                  int,int,void* d,unsigned,unsigned,unsigned,void*,unsigned,void*){
    const size_t size=size_t(w)*h*(fmt==SCE_GXM_TRANSFER_FORMAT_RAW64?8:16);
    Require(Range(d,size)&&Range(s,size),"native transfer stays within live source and destination ranges");
    pending.push_back({d,s,size});++metrics.transfers;return 0;
}
extern "C" int sceGxmTransferFinish(){
    if(!capture)return 0;++metrics.waits;
    for(const auto& p:pending){Require(Range(p.src,p.size)&&Range(p.dst,p.size),"completion references live allocations");std::memcpy(p.dst,p.src,p.size);}
    pending.clear();return 0;
}
extern "C" void __real_glCompressedTexImage2D(GLenum,GLint,GLenum,GLsizei,GLsizei,GLint,GLsizei,const void*);
void FixtureNativeCompressed(GLenum target,GLint l,GLenum f,GLsizei w,GLsizei h,GLint border,GLsizei n,const void* data){
    if(capture){calls.push_back({l,w,h,n,true,data});if(nativeUpload)gpu_alloc_compressed_texture(l,w,h,SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR,n,data,&nativeTexture,0,false);}
    __real_glCompressedTexImage2D(target,l,f,w,h,border,n,data);
}
#define glCompressedTexImage2D FixtureNativeCompressed
#include "frozen-upload-host.inc"
#undef glCompressedTexImage2D
extern "C" void __wrap_glCompressedTexImage2D(GLenum t,GLint l,GLenum f,GLsizei w,GLsizei h,GLint b,GLsizei n,const void* p){vgxvita_glCompressedTexImage2D(t,l,f,w,h,b,n,p);}
extern "C" void __real_glTexImage2D(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
extern "C" void __wrap_glTexImage2D(GLenum t,GLint l,GLint i,GLsizei w,GLsizei h,GLint b,GLenum f,GLenum type,const void* p){
    if(capture)calls.push_back({l,w,h,int(w*h*(f==GL_LUMINANCE_ALPHA?2:4)),false,p});__real_glTexImage2D(t,l,i,w,h,b,f,type,p);
}
extern "C" void __real_glGenerateMipmap(GLenum);
extern "C" void __wrap_glGenerateMipmap(GLenum t){if(capture)++mipGens;if(!skipGeneration)__real_glGenerateMipmap(t);}

int main(int argc,char** argv){
    Require(argc==2,"choose texture heap/hazard scenario");const std::string mode=argv[1];
    Context();Require(vgx::backend::Init(32,32),"actual backend initialized");
    // Native vitaGL always consumes packed input, unlike the Mesa alignment default.
    glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    Require(std::strstr(reinterpret_cast<const char*>(glGetString(GL_VERSION)),"OpenGL ES 3"),"Mesa context supports explicit LOD mip validation");
    vgx::backend::g_hasS3tc=true;
#if VGX_COLDTRACE
    Require(vgx::coldtrace::g_recorder.Arm(TraceClock,TraceThread,1),"optional RAM tracing enabled");
#endif
    if(mode=="hazard"){
        Reset("move",false);nativeUpload=capture=allowHazard=true;auto up=Chain(64,64);bool caught=false;
        try{for(size_t l=0;l<up.levels.size();++l)FixtureNativeCompressed(GL_TEXTURE_2D,l,GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,up.width>>l,up.height>>l,0,up.levels[l].size(),up.levels[l].data());}
        catch(const TransferHazard&){caught=true;}
        Require(caught,"wait-free ascending control exposes a pending-transfer realloc hazard");pending.clear();capture=nativeUpload=allowHazard=false;
    }else if(mode=="compact"){
        CompactPixels();
    }else if(mode=="controls"){
        Controls();
    }else{
        Require(mode=="inplace"||mode=="move"||mode=="fallback","known heap scenario");
        const unsigned shapes[][2]={{4,4},{8,8},{16,16},{32,32},{64,64},{256,256},{1024,1024},{256,64},{64,256},{16,64}};
        for(const auto& shape:shapes)for(bool deferred:{false,true})for(bool redefine:{false,true}){
            const auto up=Chain(shape[0],shape[1]);const auto a=Run(up,false,mode,deferred,redefine),b=Run(up,true,mode,deferred,redefine);
            Require(a.bytes==b.bytes&&a.mipCount==b.mipCount,"native reordered chain equals ascending baseline");
            Require(a.metrics.grows==up.levels.size()-1,"ascending baseline performs one growth per added level");
            Require(b.metrics.grows==(nativeOrdering?(up.levels.size()>1?1u:0u):up.levels.size()-1),"native mip upload grows its allocation at most once");
            if(nativeOrdering&&up.levels.size()>2) {
                Require(b.calls[1].level==int(up.levels.size()-1),"native allocation reaches final level immediately after base");
                int middle=1;
                for(size_t i=2;i<b.calls.size();++i)Require(b.calls[i].level==middle++,"remaining native levels ascend without gaps");
            } else for(size_t i=0;i<b.calls.size();++i)Require(b.calls[i].level==int(i),"desktop and short chains retain ascending upload order");
            Require(b.metrics.moved<=a.metrics.moved&&b.metrics.fallback<=a.metrics.fallback&&b.metrics.peak<=a.metrics.peak,"allocation copy work and logical peak never increase");
            if(shape[0]==1024&&shape[1]==1024&&!deferred&&!redefine) {
                char report[512];const int n=std::snprintf(report,sizeof report,
                    "{\"mode\":\"%s\",\"trace\":%d,\"levels\":%zu,\"baseline_grows\":%zu,\"optimized_grows\":%zu,\"baseline_copied\":%zu,\"optimized_copied\":%zu,\"baseline_peak\":%zu,\"optimized_peak\":%zu,\"transfers\":%zu,\"waits\":%zu,\"physical_vita\":false}\n",
                    mode.c_str(),VGX_COLDTRACE,up.levels.size(),a.metrics.grows,b.metrics.grows,a.metrics.moved+a.metrics.fallback,b.metrics.moved+b.metrics.fallback,a.metrics.peak,b.metrics.peak,b.metrics.transfers,b.metrics.waits);
                ::write(STDOUT_FILENO,report,n);
            }
        }
        Controls();
    }
    Require(glGetError()==GL_NO_ERROR,"texture fixture leaves real GL valid");
#if VGX_COLDTRACE
    vgx::coldtrace::g_recorder.RequestFreeze();const auto trace=vgx::coldtrace::g_recorder.Frozen();
    Require(trace.data&&(mode=="hazard"||trace.data->worstCount>0)&&!trace.clockErrors,"opt-in RAM trace captures uploads without clock errors");
#endif
    char report[256];const int n=std::snprintf(report,sizeof report,"PASS:texture-upload mode=%s trace=%d; real Mesa mips; frozen allocator; SDK/heap doubles\n",mode.c_str(),VGX_COLDTRACE);::write(STDOUT_FILENO,report,n);
}
