#define main quiet_fixture_main
#include "vgx_backend_quiet.cpp"
#undef main
#include "present-sources.inc"
namespace reference {
#include "vgx_present_reference.inc"
}

static unsigned presentDraws=0;
extern "C" void __real_glDrawArrays(GLenum,GLint,GLsizei);
extern "C" void __wrap_glDrawArrays(GLenum mode,GLint first,GLsizei count) {
    ++presentDraws;__real_glDrawArrays(mode,first,count);
}
static GLuint PixelProgram(const char* fs,const char* vs=pixelVS) {
    const auto compile=[](GLenum type,const char* source) {
        GLuint shader=glCreateShader(type);glShaderSource(shader,1,&source,nullptr);glCompileShader(shader);
        GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
        if(!ok){char text[4096];glGetShaderInfoLog(shader,sizeof text,nullptr,text);std::fprintf(stderr,"%s\n",text);}
        Require(ok,"pixel shader compiled");return shader;
    };
    GLuint v=compile(GL_VERTEX_SHADER,vs),f=compile(GL_FRAGMENT_SHADER,fs),p=glCreateProgram();
    glAttachShader(p,v);glAttachShader(p,f);glBindAttribLocation(p,vgx::AT_POS,"a_pos");glLinkProgram(p);
    GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);Require(ok,"pixel program linked");
    glDeleteShader(v);glDeleteShader(f);return p;
}
static vgx::backend::SharpProg Select(GLuint program) {
    using namespace vgx::backend;
    SharpProg p;p.prog=program;
    p.rect=glGetUniformLocation(program,"u_rect");p.size=glGetUniformLocation(program,"u_size");
    p.lim=glGetUniformLocation(program,"u_lim");p.p=glGetUniformLocation(program,"u_p");
    Require(p.rect>=0&&p.size>=0&&p.lim>=0&&p.p>=0,"all present uniforms retained");
    glUseProgram(program);glUniform1i(glGetUniformLocation(program,"u_src"),0);Invalidate();return p;
}
int main() {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    Context();
    std::printf("GL=%s; half is pack/unpack emulation, not native GXP\n",glGetString(GL_VERSION));
    using namespace vgx::backend;
    Require(EnsureSharpenProgram(),"actual baseline program builds");
    const SharpProg actual=g_sharp,original=Select(PixelProgram(reference::fs,reference::vs)),control=Select(PixelProgram(pixelFloatFS)),half=Select(PixelProgram(pixelHalfFS)),strictControl=Select(PixelProgram(pixelStrictFloatFS));
    std::printf("programs original=%u control=%u half=%u\n",original.prog,control.prog,half.prog);
    const float quad[]={-1,-1,1,-1,-1,1,1,1};
    glGenBuffers(1,&g_quadVbo);glBindBuffer(GL_ARRAY_BUFFER,g_quadVbo);glBufferData(GL_ARRAY_BUFFER,sizeof quad,quad,GL_STATIC_DRAW);
    constexpr int dw=960,dh=544;
    GLuint output,fbo;glGenTextures(1,&output);glBindTexture(GL_TEXTURE_2D,output);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,dw,dh,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,output,0);
    Require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"destination RGBA8 FBO complete");
    glGenTextures(1,&g_efbColor);glBindTexture(GL_TEXTURE_2D,g_efbColor);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    const int sizes[][2]={{640,456},{720,408},{800,454},{960,544}};
    uint64_t comparisons=0,changed=0;unsigned worst=0,hostWorst=0,renders=0;
    for(auto& size:sizes)for(int pattern=0;pattern<6;++pattern) {
        const bool cropped=pattern%2==0;
        g_rtW=size[0];g_rtH=size[1];const int x=cropped?3:0,y=cropped?5:0,w=g_rtW-2*x,h=g_rtH-2*y;
        std::vector<unsigned char> source(g_rtW*g_rtH*4);uint32_t seed=0x12345678;
        for(int row=0;row<g_rtH;++row)for(int col=0;col<g_rtW;++col)for(int c=0;c<4;++c) {
            seed=seed*1664525+1013904223;unsigned v=0;
            switch(pattern){case 0:v=77;break;case 1:v=(col*255)/(g_rtW-1);break;
            case 2:v=((col+row)&1)?255:0;break;case 3:v=col==g_rtW/2&&row==g_rtH/2?255:0;break;
            case 4:v=col<g_rtW/2?0:255;break;default:v=seed>>24;}
            if(col<x||col>=x+w||row<y||row>=y+h)v=c==0?255:0;
            source[(row*g_rtW+col)*4+c]=c==3?unsigned((row+col)&255):v;
        }
        glBindTexture(GL_TEXTURE_2D,g_efbColor);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,g_rtW,g_rtH,0,GL_RGBA,GL_UNSIGNED_BYTE,source.data());
        for(float amount:{0.0f,0.8f,1.5f}) {
            const bool flip=amount!=0.8f;
            const auto render=[&](SharpProg p) {
                g_sharp=p;Invalidate();const unsigned draws=presentDraws;C=Counts{};
                Require(PresentSharpened(fbo,x,y,w,h,0,0,dw,dh,amount,flip),"actual present path succeeds");
                float applied[4];glGetUniformfv(p.prog,p.p,applied);Require(applied[2]==amount&&applied[3]==0,"strength and emulation barrier uploaded");
                Require(presentDraws==draws+1&&C.uniformCalls==4&&C.uniformBytes==64,"one pass and four full uniforms");
                std::vector<unsigned char> pixels(dw*dh*4);glReadPixels(0,0,dw,dh,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
                Require(glGetError()==GL_NO_ERROR,"present pixel read has no GL error");++renders;return pixels;
            };
            auto a=render(original),b=render(control),c=render(half),d=render(strictControl),e=render(actual);
            if(pattern==5)std::printf("noise strength=%f source=%u,%u output=%u,%u half=%u,%u\n",amount,source[0],source[1000],a[0],a[1000],c[0],c[1000]);
            Require(a==b,"GLSL3 mechanical control equals actual GLSL1 baseline exactly");
            for(size_t i=0;i<a.size();++i) {
                unsigned delta=unsigned(std::abs(int(d[i])-int(c[i])));worst=std::max(worst,delta);changed+=delta!=0;++comparisons;
                const unsigned hostDelta=unsigned(std::abs(int(a[i])-int(e[i])));hostWorst=std::max(hostWorst,hostDelta);
                Require(hostDelta<=1&&((i%4!=3)||e[i]==255),"actual desktop variant stays within original pixel bound");
                Require((i%4==3)?a[i]==255&&c[i]==255&&d[i]==255:delta<=1,"highp-sampler model RGB error <=1 UNORM8; alpha stays opaque");
                if(cropped&&pattern==0&&i%4!=3)Require(a[i]==77&&c[i]==77&&d[i]==77,"crop excludes outside red sentinel");
            }
        }
        std::printf("completed source=%dx%d pattern=%d crop=%d\n",g_rtW,g_rtH,pattern,int(cropped));
    }
    Require(changed>0,"explicit half quantization is exercised");
    std::printf("PASS renders=%u compared_channels=%llu changed=%llu max_delta=%u host_default_max_delta=%u sizes=4 patterns=6 crop_and_flip=pairwise strengths=3 original_vs_mechanical_lowp=exact highp_sampler_half_model=explicit draw_calls=%u physical_vita=false\n",renders,
        static_cast<unsigned long long>(comparisons),static_cast<unsigned long long>(changed),worst,hostWorst,presentDraws);
}
