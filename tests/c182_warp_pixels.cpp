// Draw the production mask, reprojection, silhouette-key, and optional occlusion programs in GLES.
// A known synthetic two-depth scene provides an independent redraw, not a screenshot of gameplay.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
bool EdgeMirrorEnv(){return false;} bool BoxRoundEnv(){return false;}
bool Mask41User(){return true;}
#ifdef C185_DEPTH_EDGES
bool depth41Edge = false;
int depth41Mode = 1;
bool depth41Debug = false;
int Depth41Env(){return depth41Mode;}
bool Depth41DebugEnv(){return depth41Debug;}
bool Depth41EdgeEnv(){return depth41Edge;}
#else
int Depth41Env(){return 1;}
bool Depth41DebugEnv(){return false;}
bool Depth41EdgeEnv(){return false;}
#endif
#include "warp_sources.inc"
#include "port/keepdepth.h"  // (keepdepth31: kFsBoxDepth against its pure rule)

namespace {
constexpr int W=128,H=64;
using Pixel=std::array<unsigned char,4>;
using Image=std::vector<Pixel>;
const char* receipts=nullptr;
void check(bool ok,const char* why){if(!ok){std::fprintf(stderr,"FAIL %s (GL %x, EGL %x)\n",why,glGetError(),eglGetError());std::exit(1);}}
struct Context {
    EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY); EGLContext ctx; EGLSurface surf;
    Context(){
        check(eglInitialize(d,nullptr,nullptr),"initialize software EGL");
        check(eglBindAPI(EGL_OPENGL_ES_API),"bind GLES2");
        EGLConfig cfg; EGLint num;
        EGLint attrs[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,
            EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
        check(eglChooseConfig(d,attrs,&cfg,1,&num)&&num==1,"RGBA8 config");
        EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE}, sa[]={EGL_WIDTH,W,EGL_HEIGHT,H,EGL_NONE};
        ctx=eglCreateContext(d,cfg,EGL_NO_CONTEXT,ca); surf=eglCreatePbufferSurface(d,cfg,sa);
        check(eglMakeCurrent(d,surf,surf,ctx),"current pbuffer");
        std::printf("Renderer: %s (%s)\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
        glDisable(GL_DITHER);
    }
    ~Context(){eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroySurface(d,surf);eglDestroyContext(d,ctx);eglTerminate(d);}
};
GLuint program(const std::string& vs,const std::string& fs){
    GLuint p=glCreateProgram();
    for(auto entry:{std::make_pair(GL_VERTEX_SHADER,&vs),std::make_pair(GL_FRAGMENT_SHADER,&fs)}){
        GLuint s=glCreateShader(entry.first);const char* src=entry.second->c_str();
        glShaderSource(s,1,&src,nullptr);glCompileShader(s);
        int ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
        if(!ok){char msg[2048];glGetShaderInfoLog(s,sizeof msg,nullptr,msg);check(false,msg);}
        glAttachShader(p,s);glDeleteShader(s);
    }
    glBindAttribLocation(p,0,"a_pos");glLinkProgram(p);int ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);check(ok,"link production program");return p;
}
GLuint texture(int unit,int w,int h,const Pixel* data,bool linear=false){
    GLuint id;glGenTextures(1,&id);glActiveTexture(GL_TEXTURE0+unit);glBindTexture(GL_TEXTURE_2D,id);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,linear?GL_LINEAR:GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,linear?GL_LINEAR:GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,data);return id;
}
void u(GLuint p,const char* name,float a,float b=0,float c=0,float d=0){glUniform4f(glGetUniformLocation(p,name),a,b,c,d);}
void draw(){
    static constexpr GLfloat v[]={-1,-1,1,-1,-1,1,1,1};glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,0,0,v);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);check(glGetError()==GL_NO_ERROR,"draw");
}
Image read(const char* name){
    Image img(W*H);glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,img.data());check(glGetError()==GL_NO_ERROR,"readback");
    if(receipts){
        auto path=std::string(receipts)+"/"+name+".ppm";FILE* f=std::fopen(path.c_str(),"wb");check(f,"open image receipt");
        std::fprintf(f,"P6\n%d %d\n255\n",W,H);for(int y=H-1;y>=0;y--)for(int x=0;x<W;x++)std::fwrite(img[y*W+x].data(),1,3,f);std::fclose(f);
    }
    return img;
}
void setup(GLuint p,GLuint col,GLuint mask){
    glUseProgram(p);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,col);glUniform1i(glGetUniformLocation(p,"u_src"),0);
    glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,mask);glUniform1i(glGetUniformLocation(p,"u_depth"),1);
    u(p,"u_a",.5,0,0,.5);u(p,"u_b",0,.5,0,.5);u(p,"u_c",0,0,0,1);u(p,"u_k",.5f/W,.5f/H,1-.5f/W,1-.5f/H);
    glViewport(0,0,W,H);
}
GLuint makeMask(GLuint p,const Image& depth){
    GLuint raw=texture(0,W,H,depth.data()),mask=texture(1,W/4,H/4,nullptr,true),fb;
    glGenFramebuffers(1,&fb);glBindFramebuffer(GL_FRAMEBUFFER,fb);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,mask,0);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"quarter-size mask FBO");
    glDisable(GL_BLEND);setup(p,raw,0);glViewport(0,0,W/4,H/4);u(p,"u_c",1,-1,0,-64);u(p,"u_o",1.f/W,1.f/H);draw();
    glBindFramebuffer(GL_FRAMEBUFFER,0);glDeleteTextures(1,&raw);glDeleteFramebuffers(1,&fb);return mask;
}
int err(const Pixel& a,const Pixel& b){return std::abs(int(a[0])-b[0])+std::abs(int(a[1])-b[1])+std::abs(int(a[2])-b[2]);}
float code(float d){return (std::log2(d)-5)/15;}
// keepdepth31 (GXV_HRW_BOXDEPTH): the production kFsBoxDepth drawn over a jump in front of which a rail (two rows, nearer than him) crosses him. The
// depth is the first picture's own, full size, read as the window depth in red (the EFB depth texture's convention; here 8 bits). Every box pixel is
// compared with port::keepdepth::BoxDepthWeight computed on the CPU from the same bytes (the shader is the rule), and the three boxes with the truth:
// the rail stays where it is, the background stays, he moves.
void BoxDepth(GLuint maskP,GLuint rot,GLuint box,GLuint keyed){
    const GLuint full=program(kVsBoxKey,kFsBoxDepth);
    Image src(W*H),depth(W*H);
    auto him=[](int x,int y,int dy){return x>=58&&x<70&&y>=22+dy&&y<34+dy;};
    auto rail=[](int y){return y==27||y==28;};
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        unsigned char bg=(y/3)%2?200:40;
        src[y*W+x]=rail(y)?Pixel{18,80,225,255}:him(x,y,0)?Pixel{225,(unsigned char)(25+4*(y-22)),10,255}:Pixel{bg,bg,bg,255};
        unsigned char z=rail(y)?128:him(x,y,0)?191:239;depth[y*W+x]={z,z,z,255};
    }
    GLuint col=texture(0,W,H,src.data(),true),raw=texture(1,W,H,depth.data(),false),mask=makeMask(maskP,depth);
    // window depth -> distance under the fixture's projection (e = -1, f = -64: the mask's u_c): 191 -> 127.5 units (him), 128 -> 64 (the rail), 239 -> 510
    const double e=-1.0,f=-64.0,hisDist=-f/-((2.0*191/255-1.0)+e);
    port::hrw41::Key k;k.code=static_cast<float>(port::hrw41::DepthCode(hisDist));k.tol=.007f;k.slope=2.f/.007f;
    auto run=[&](GLuint p,bool fullKey,const char* name){
        glDisable(GL_BLEND);setup(rot,col,mask);u(rot,"u_z",0);u(rot,"u_p",0);u(rot,"u_a",.5,0,0,.5);draw();  // (the scene's warp: the camera stays)
        setup(p,col,fullKey?raw:mask);glViewport(32,8,64,48);
        u(p,"u_a",.25,0,0,.5);u(p,"u_b",0,.375,0,.5-8.f/H);u(p,"u_c",0,0,0,1);
        u(p,"u_h",.25,0,0,.5);u(p,"u_i",0,.375,0,.5);u(p,"u_j",0,0,0,1);
        if(fullKey){u(p,"u_g",k.code,k.tol,k.slope);u(p,"u_q",static_cast<float>(e),static_cast<float>(-f),1,0);}
        else{u(p,"u_g",code(128),.007f,800.f);u(p,"u_m",2.f/W,2.f/H,W/4,H/4);}
        u(p,"u_z",0,0,0,18);u(p,"u_d",0,18);
        glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);draw();glDisable(GL_BLEND);return read(name);
    };
    const auto plain=run(box,false,"railjump-plain-box"),quarter=run(keyed,false,"railjump-quarter-mask-key"),fk=run(full,true,"railjump-full-depth-key");
    int model=0,railPlain=0,railQuarter=0,railFull=0,bgPlain=0,bgFull=0,n=0;
    for(int y=8;y<56;y++)for(int x=32;x<96;x++){
        // the CPU model of the same pixel: the box's source one 8 rows down, the scene's own source the pixel itself, the fade as the program's
        const double ax=(x+.5-64)/32,ay=(y+.5-32)/24;
        const double fade=std::clamp(std::min((1-std::fabs(ax))*18,(1-std::fabs(ay))*18),0.0,1.0);
        const int sy=y-8;const Pixel& s=src[(sy<0?0:sy)*W+x];const Pixel& d=src[y*W+x];
        const double a=fade*port::keepdepth::BoxDepthWeight(k,depth[(sy<0?0:sy)*W+x][0]/255.0,depth[y*W+x][0]/255.0,e,f);
        Pixel m{};for(int c=0;c<3;c++)m[c]=static_cast<unsigned char>(std::lround(a*s[c]+(1-a)*d[c]));
        model=std::max(model,err(fk[y*W+x],m));
        // the truth (where the first picture has it): the rail and the background stay, he moves 8 rows up (rows hidden behind the rail in A: not counted)
        if(rail(y)){railPlain+=err(plain[y*W+x],src[y*W+x])>15;railQuarter+=err(quarter[y*W+x],src[y*W+x])>15;railFull+=err(fk[y*W+x],src[y*W+x])>15;}
        else if(!him(x,y,8)&&!him(x,y,0)&&fade>=1){bgPlain+=err(plain[y*W+x],src[y*W+x])>15;bgFull+=err(fk[y*W+x],src[y*W+x])>15;n++;}
    }
    const Pixel moved=fk[38*W+64];
    std::printf("box depth: worst difference from port::keepdepth::BoxDepthWeight %d levels (RGB sum); rail pixels moved: plain box %d, quarter-mask key %d, "
                "full-size depth key %d; background pixels moved (%d checked): plain %d, full-size %d; moved player RGB %u,%u,%u\n",
                model,railPlain,railQuarter,railFull,n,bgPlain,bgFull,moved[0],moved[1],moved[2]);
    check(model<=6,"kFsBoxDepth draws port::keepdepth::BoxDepthWeight");
    check(railPlain>40&&railFull==0&&bgPlain>100&&bgFull==0,"the full-size depth key keeps the rail in front and the background still");
    check(moved[0]>190&&moved[2]<60,"he still moves with the box");
}
}
#ifdef C185_DEPTH_EDGES
#include "c185_depth_edges.inc"
#endif
int main(int argc,char** argv){
    if(argc>1) receipts=argv[1];
    Context ctx;
    const auto maskP=program(kVs,MaskFragment()),rot=program(kVsWarp,WarpFragment(0)),d41=program(kVsWarp,D41Fragment());
    const auto keyed=program(kVsBoxKey,kFsBoxKey),occ=program(kVsWarp,D41Fragment(true));
    std::string boxVS=kVsWarp,from="v_tex2 = v_uv + u_e.xy;";auto i=boxVS.find(from);check(i!=std::string::npos,"box vertex setup");
    boxVS.replace(i,from.size(),"v_tex2 = a_pos.xy;");auto box=program(boxVS,WarpFragment(kWarpBox));
    Image src(W*H),depth(W*H);
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        unsigned char c=((x/3)%2)?210:30;src[y*W+x]={c,c,c,255};
        // Distances 64 and 256 under the fixture's projection, quantized to R8 window depth.
        unsigned char z=y<H/2?128:223;depth[y*W+x]={z,z,z,255};
    }
    GLuint col=texture(0,W,H,src.data(),true),mask=makeMask(maskP,depth);
    auto pan=[&](GLuint p,const char* name){
        setup(p,col,mask);u(p,"u_a",.5,0,0,.5f+2.f/W);u(p,"u_z",8.f/W);u(p,"u_p",0,64,0,0);draw();return read(name);
    };
    auto before=pan(rot,"pan-rotation-only"),after=pan(d41,"pan-depth");
    int n=0;double e0=0,e1=0;
    for(int y=5;y<H-5;y++)if(std::abs(y-H/2)>5)for(int x=8;x<W-15;x++){
        const Pixel& truth=src[y*W+x+2+(y<H/2?8:2)];e0+=err(before[y*W+x],truth);e1+=err(after[y*W+x],truth);n++;
    }
    std::printf("pan: %d interior pixels; mean absolute RGB error vs redraw: %.3f -> %.3f levels/channel\n",n,e0/(3*n),e1/(3*n));
    check(e0>n*40&&e1<.08*e0,"two-depth pan tracks independent redraw");
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){
        const bool player=x>=58&&x<70&&y>=22&&y<34;
        const bool wall=x>=84&&x<100;unsigned char bg=(y/3)%2?200:40;
        src[y*W+x]=player?Pixel{225,25,10,255}:wall?Pixel{18,80,225,255}:Pixel{bg,bg,bg,255};
        unsigned char z=player?191:wall?128:239;depth[y*W+x]={z,z,z,255};
    }
    glDeleteTextures(1,&col);glDeleteTextures(1,&mask);col=texture(0,W,H,src.data(),true);mask=makeMask(maskP,depth);
    auto jump=[&](GLuint p,const char* name){
        glDisable(GL_BLEND);setup(rot,col,mask);u(rot,"u_z",0);u(rot,"u_p",0);u(rot,"u_a",.5,0,0,.5);draw();
        setup(p,col,mask);glViewport(32,8,64,48);
        u(p,"u_a",.25,0,0,.5);u(p,"u_b",0,.375,0,.5-8.f/H);u(p,"u_c",0,0,0,1);
        u(p,"u_h",.25,0,0,.5);u(p,"u_i",0,.375,0,.5);u(p,"u_j",0,0,0,1);
        u(p,"u_g",code(128),.007f,800.f);u(p,"u_m",2.f/W,2.f/H,W/4,H/4);u(p,"u_z",0,0,0,18);u(p,"u_d",0,18);
        glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);draw();glDisable(GL_BLEND);return read(name);
    };
    auto jb=jump(box,"jump-unkeyed"),ja=jump(keyed,"jump-depth-keyed");
    int dirtyB=0,dirtyA=0,checks=0;
    for(int y=12;y<52;y++)for(int x=36;x<92;x++)if(x<52||x>=76){
        dirtyB+=err(jb[y*W+x],src[y*W+x])>15;dirtyA+=err(ja[y*W+x],src[y*W+x])>15;checks++;
    }
    auto pix=ja[38*W+64];
    std::printf("jump: %d checked non-player surroundings; wrongly displaced pixels %d -> %d; moved-player RGB %u,%u,%u\n",checks,dirtyB,dirtyA,pix[0],pix[1],pix[2]);
    check(dirtyB>300&&dirtyA==0&&pix[0]>190&&pix[1]<60,"key protects surrounding scenery while moving player");
    // Clear to fresh-player color; reproject A's genuinely nearer foreground across it with the production occlusion program.
    glViewport(0,0,W,H);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);setup(occ,col,mask);
    u(occ,"u_d",0,code(100));glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);draw();glDisable(GL_BLEND);
    auto oc=read("fresh-player-occlusion");auto near=oc[32*W+90],self=oc[28*W+64],far=oc[32*W+15];
    check(near[2]>210&&self[0]==255&&self[1]==0&&far[0]==255&&far[1]==0,"foreground restored without restoring self/far");
    glClear(GL_COLOR_BUFFER_BIT);setup(occ,col,mask);u(occ,"u_z",-40.f/W);u(occ,"u_p",0,0,0,0);u(occ,"u_d",0,code(100));
    // zn=1 for this pass: predicted wall maps onto background; predicted background maps onto wall.
    u(occ,"u_p",-1,0);glEnable(GL_BLEND);draw();glDisable(GL_BLEND);auto disagreement=read("ambiguous-occlusion");
    check(disagreement[32*W+90][0]==255&&disagreement[32*W+127][0]==255,"both disagreement directions must reject");
    std::puts("foreground: near restored, behind/self and two inconsistent depth samples left untouched. Synthetic raster test only.");
    BoxDepth(maskP,rot,box,keyed);
#ifdef C185_DEPTH_EDGES
    DepthEdges();
#endif
}
