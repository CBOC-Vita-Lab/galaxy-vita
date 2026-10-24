// Real Mesa fragment comparison: integer SSBO TEV vs portable float uniforms.
#include <EGL/egl.h>
#include <GLES3/gl32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "portable_shader.h"

static constexpr int width = 32, height = 32;
static void check(bool valid, const char* message) {
    if (!valid) { fprintf(stderr, "portable TEV FAILED: %s (GL %x)\n", message, glGetError()); exit(1); }
}

static GLuint compile(GLenum type, const std::string& source) {
    GLuint shader = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr); glCompileShader(shader);
    GLint valid = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &valid);
    if (!valid) { char log[4096]; glGetShaderInfoLog(shader, sizeof(log), nullptr, log); fprintf(stderr, "%s\n%s\n", log, text); }
    check(valid, "shader compile");
    return shader;
}

static GLuint program(const std::string& vs, const std::string& fs, bool es100) {
    GLuint vertex = compile(GL_VERTEX_SHADER, vs), fragment = compile(GL_FRAGMENT_SHADER, fs);
    GLuint result = glCreateProgram();
    glAttachShader(result, vertex); glAttachShader(result, fragment);
    if (es100) {
        glBindAttribLocation(result, 0, "a_clip");
        glBindAttribLocation(result, 1, "a_clr0"); glBindAttribLocation(result, 2, "a_clr1");
        for (int i = 0; i < 8; ++i) glBindAttribLocation(result, 3 + i, ("a_tex" + std::to_string(i)).c_str());
        glBindAttribLocation(result, 11, "a_depth");
    }
    glLinkProgram(result);
    GLint linked = 0; glGetProgramiv(result, GL_LINK_STATUS, &linked);
    if (!linked) { char log[4096]; glGetProgramInfoLog(result, sizeof(log), nullptr, log); fprintf(stderr, "%s\n", log); }
    check(linked, "shader link");
    glDeleteShader(vertex); glDeleteShader(fragment);
    return result;
}

static void context() {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    check(eglInitialize(display, nullptr, nullptr), "EGL initialize");
    const EGLint attributes[]{EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_NONE};
    EGLConfig config; EGLint count;
    check(eglChooseConfig(display,attributes,&config,1,&count) && count, "EGL config");
    const EGLint version[]{EGL_CONTEXT_MAJOR_VERSION,3,EGL_CONTEXT_MINOR_VERSION,2,EGL_NONE};
    EGLContext ctx = eglCreateContext(display,config,EGL_NO_CONTEXT,version);
    const EGLint dimensions[]{EGL_WIDTH,width,EGL_HEIGHT,height,EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display,config,dimensions);
    check(ctx != EGL_NO_CONTEXT && eglMakeCurrent(display,surface,surface,ctx), "GLES context");
    printf("portable TEV reference: %s / %s; not Vita GPU evidence\n",glGetString(GL_VERSION),glGetString(GL_RENDERER));
}

static std::string referenceVs(const gpu::ShaderUid& uid) {
    std::string text = "#version 320 es\nprecision highp float;\nprecision highp int;\nlayout(location=0) in vec4 a_clip;\nlayout(location=1) in vec4 a_clr0;\nlayout(location=2) in vec4 a_clr1;\nlayout(location=11) in vec2 a_depth;\nflat out uint v_pix;\nout vec4 v_clr0;\nout vec4 v_clr1;\nout float v_viewz;\nout float v_gxz;\n";
    for (uint32_t i = 0; i < uid.numTexGens; ++i)
        text += "layout(location=" + std::to_string(3+i) + ") in vec3 a_tex" + std::to_string(i) + ";\nout vec3 v_tex" + std::to_string(i) + ";\n";
    text += "void main(){gl_Position=a_clip;v_pix=0u;v_clr0=a_clr0;v_clr1=a_clr1;v_viewz=a_depth.x;v_gxz=a_depth.y;";
    for (uint32_t i = 0; i < uid.numTexGens; ++i) text += "v_tex" + std::to_string(i) + "=a_tex" + std::to_string(i) + ";";
    return text + "}\n";
}

struct Vertex { float clip[4], color[2][4], tex[8][3], depth[2]; };
static GLuint vao = 0, vbo = 0, ssbo = 0, framebuffer = 0, colorTexture = 0, depthTexture = 0;
static GLuint depthReadFramebuffer = 0, depthReadTexture = 0, depthReadProgram = 0;
static std::array<GLuint,8> textures;
static std::mt19937 generator(0x544556);
static float normalized() { return float(generator() % 256) / 255; }

static void setup() {
    glGenVertexArrays(1,&vao); glBindVertexArray(vao);
    glGenBuffers(1,&vbo); glBindBuffer(GL_ARRAY_BUFFER,vbo);
    auto attribute = [](GLuint loc,int count,size_t offset) {
        glEnableVertexAttribArray(loc); glVertexAttribPointer(loc,count,GL_FLOAT,GL_FALSE,sizeof(Vertex),reinterpret_cast<void*>(offset));
    };
    attribute(0,4,offsetof(Vertex,clip)); attribute(1,4,offsetof(Vertex,color)); attribute(2,4,offsetof(Vertex,color)+16);
    for (int i=0;i<8;++i) attribute(3+i,3,offsetof(Vertex,tex)+i*12);
    attribute(11,2,offsetof(Vertex,depth));
    glGenBuffers(1,&ssbo); glBindBuffer(GL_SHADER_STORAGE_BUFFER,ssbo); glBindBufferBase(GL_SHADER_STORAGE_BUFFER,2,ssbo);
    glGenTextures(8,textures.data());
    for (int t=0;t<8;++t) {
        glActiveTexture(GL_TEXTURE0+t); glBindTexture(GL_TEXTURE_2D,textures[t]);
        for (int level=0;level<4;++level) {
            const int size=8>>level;
            std::vector<unsigned char> data(size*size*4);
            for (auto& byte:data) byte=generator();
            glTexImage2D(GL_TEXTURE_2D,level,GL_RGBA8,size,size,0,GL_RGBA,GL_UNSIGNED_BYTE,data.data());
        }
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST_MIPMAP_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    }
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1,&colorTexture); glBindTexture(GL_TEXTURE_2D,colorTexture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,width,height,0,GL_RGBA,GL_FLOAT,nullptr);
    glGenTextures(1,&depthTexture); glBindTexture(GL_TEXTURE_2D,depthTexture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT32F,width,height,0,GL_DEPTH_COMPONENT,GL_FLOAT,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glGenFramebuffers(1,&framebuffer); glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,colorTexture,0);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,depthTexture,0);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"float/depth framebuffer");
    glGenTextures(1,&depthReadTexture); glBindTexture(GL_TEXTURE_2D,depthReadTexture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,width,height,0,GL_RGBA,GL_FLOAT,nullptr);
    glGenFramebuffers(1,&depthReadFramebuffer); glBindFramebuffer(GL_FRAMEBUFFER,depthReadFramebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,depthReadTexture,0);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"depth readback framebuffer");
    depthReadProgram=program("#version 320 es\nprecision highp float;\nlayout(location=0) in vec4 a_clip;\nvoid main(){gl_Position=a_clip;}",
        "#version 320 es\nprecision highp float;\nuniform highp sampler2D depthImage;\nout vec4 color;\nvoid main(){color=vec4(texelFetch(depthImage,ivec2(gl_FragCoord.xy),0).r);}",false);
    glViewport(0,0,width,height); glEnable(GL_DEPTH_TEST); glDepthFunc(GL_ALWAYS);
    check(glGetError()==GL_NO_ERROR,"setup");
}

static gpu::PixelState pixelState() {
    gpu::PixelState state{};
    for (auto& reg:state.tevReg) for (float& value:reg) value=float(int(generator()%2048)-1024)/255;
    for (auto& reg:state.konst) for (float& value:reg) value=normalized();
    state.alphaRef[0]=generator()%256; state.alphaRef[1]=generator()%256; state.alphaRef[2]=generator()&0xFFFFFF;
    for (float& value:state.fogColor) value=normalized();
    state.fogParams[0]=0.7f; state.fogParams[1]=0.1f; state.fogParams[2]=20000000; state.fogParams[3]=1;
    for (auto& size:state.texSize) { size[0]=float(int(generator()%5)-2); size[1]=8; size[2]=size[3]=0.125f; }
    for (auto& matrix:state.indMtx) for (auto& row:matrix) for (float& value:row) value=float(int(generator()%101)-50)/1024;
    return state;
}

static std::array<Vertex,3> vertices() {
    std::array<Vertex,3> result{};
    const float xy[3][2]={{-1,-1},{3,-1},{-1,3}};
    for (int i=0;i<3;++i) {
        result[i].clip[0]=xy[i][0]; result[i].clip[1]=xy[i][1]; result[i].clip[2]=float(i-1)*0.3f; result[i].clip[3]=1;
        for (auto& color:result[i].color) for (float& value:color) value=normalized();
        for (int t=0;t<8;++t) { result[i].tex[t][0]=float(i==1)*3.0f+float(t)/7; result[i].tex[t][1]=float(i==2)*3.0f; result[i].tex[t][2]=1.0f+float(i)/4; }
        result[i].depth[0]=float(i)*4; result[i].depth[1]=0.3f+float(i)*0.25f;
    }
    return result;
}

static std::vector<float> render(GLuint prog,const gpu::PixelUniformLayout* layout,const gpu::PixelState& state,
                                 const float* raster=nullptr) {
    glUseProgram(prog); glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE);
    glClearColor(-1,-1,-1,-1); glClearDepthf(1); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    if (layout) {
        std::array<float,108> packed;
        check(gpu::packPixelUniforms(*layout,state,packed),"pack pixel uniforms");
        glUniform4fv(glGetUniformLocation(prog,"u_pixel[0]"),layout->vectors,packed.data());
    } else { glBindBuffer(GL_SHADER_STORAGE_BUFFER,ssbo); glBufferData(GL_SHADER_STORAGE_BUFFER,sizeof(state),&state,GL_DYNAMIC_DRAW); }
    if (raster) glUniform4fv(glGetUniformLocation(prog,"u_raster[0]"),2,raster);
    for (int i=0;i<8;++i) {
        glActiveTexture(GL_TEXTURE0+i); glBindTexture(GL_TEXTURE_2D,textures[i]);
        glUniform1i(glGetUniformLocation(prog,("s_tex"+std::to_string(i)).c_str()),i);
    }
    glBindVertexArray(vao); glDrawArrays(GL_TRIANGLES,0,3);
    std::vector<float> pixels(width*height*8);
    glReadPixels(0,0,width,height,GL_RGBA,GL_FLOAT,pixels.data());
    glBindFramebuffer(GL_FRAMEBUFFER,depthReadFramebuffer); glDisable(GL_DEPTH_TEST);
    glUseProgram(depthReadProgram); glUniform1i(glGetUniformLocation(depthReadProgram,"depthImage"),0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,depthTexture);
    glDrawArrays(GL_TRIANGLES,0,3);
    glReadPixels(0,0,width,height,GL_RGBA,GL_FLOAT,pixels.data()+width*height*4);
    check(glGetError()==GL_NO_ERROR,"real fragment render/readback");
    return pixels;
}

static size_t compared=0, variants=0;
static uint32_t maximumVectors=0;
static float maximum=0;
static void depthEndpoints() {
    const GLuint saved=textures[1];
    GLuint zero; glGenTextures(1,&zero); glBindTexture(GL_TEXTURE_2D,zero);
    const unsigned char black[4]{};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,black);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    textures[1]=zero;
    gpu::ShaderUid uid{}; uid.numTevStages=1; uid.alphaFunc=63; uid.zTex=4; uid.tevOrder[0]=1|64;
    uid.tevColor[0]=15<<12|15<<8|15<<4|8|1<<19;
    uid.tevAlpha[0]=7<<13|7<<10|7<<7|4<<4|1<<19;
    // Identity texture swap, retained through both GLSL dialects.
    uid.ksel[0]=4; uid.ksel[1]=14;
    gpu::PixelUniformLayout layout; std::string error;
    check(gpu::makePixelUniformLayout(uid,layout,error),"depth endpoint layout");
    const GLuint reference=program(referenceVs(uid),gpu::genFragmentShader(uid),false);
    const GLuint diagnostic=program(referenceVs(uid),gpu::genPortableFragmentShader(uid,gpu::PortableDialect::Es320Diagnostic),false);
    const GLuint es100=program(gpu::genPortableCpuVertexShader(uid),gpu::genPortableFragmentShader(uid,gpu::PortableDialect::Es100),true);
    const float nearFar=std::nextafter(std::nextafter(1.0f,0.0f),0.0f);
    for (float windowZ:{0.0f,1.0f,nearFar,0.5f}) {
        auto geometry=vertices();
        for (auto& vertex:geometry) vertex.clip[2]=windowZ*2-1;
        glBindBuffer(GL_ARRAY_BUFFER,vbo); glBufferData(GL_ARRAY_BUFFER,sizeof(geometry),geometry.data(),GL_DYNAMIC_DRAW);
        const uint32_t z=uint32_t(std::floor(double(windowZ)*16777215.0+0.5));
        const float expected=float(z)/16777215.0f;
        for (GLuint p:{reference,diagnostic,es100}) {
            auto pixels=render(p,p==reference?nullptr:&layout,{});
            for (size_t i=width*height*4;i<pixels.size();++i) {
                const float tolerance=(windowZ==0 || windowZ==1)?0.0f:1.0f/16777215.0f;
                if (std::abs(pixels[i]-expected)>tolerance) fprintf(stderr,"depth endpoint program=%u windowZ=%.9g expected=%.9g actual=%.9g\n",p,windowZ,expected,pixels[i]);
                check(std::isfinite(pixels[i]) && std::abs(pixels[i]-expected)<=tolerance,"independent Z24 endpoint, zero texture/bias");
            }
        }
    }
    glDeleteProgram(reference); glDeleteProgram(diagnostic); glDeleteProgram(es100);
    const GLuint integerProbe=program("#version 320 es\nprecision highp float;\nlayout(location=0) in vec4 a_clip;\nvoid main(){gl_Position=a_clip;}",
        std::string("#version 320 es\nprecision highp float;\nuniform float windowZ;\nout vec4 color;\n")+gpu::kZ24WindowDepthGlsl+
        "void main(){color=vec4(gxWindowDepth(windowZ));}",false);
    for (float windowZ:{0.0f,0.5f,nearFar,1.0f}) {
        const float expected=float(std::floor(double(windowZ)*16777215.0+0.5));
        glUseProgram(integerProbe); glUniform1f(glGetUniformLocation(integerProbe,"windowZ"),windowZ);
        auto pixels=render(integerProbe,&layout,{});
        for (size_t i=0;i<size_t(width*height*4);++i) check(pixels[i]==expected,"raw Z24 conversion before normalized depth division");
    }
    glDeleteProgram(integerProbe);
    textures[1]=saved; glDeleteTextures(1,&zero);
    printf("portable TEV: independent zero/mid/near-far/far Z24 endpoints passed in all three programs\n");
}

static void fixture(const gpu::ShaderUid& uid) {
    gpu::PixelUniformLayout layout; std::string error;
    check(gpu::makePixelUniformLayout(uid,layout,error),"valid uniform layout");
    maximumVectors=std::max(maximumVectors,layout.vectors);
    const auto state=pixelState(); const auto geometry=vertices();
    glBindBuffer(GL_ARRAY_BUFFER,vbo); glBufferData(GL_ARRAY_BUFFER,sizeof(geometry),geometry.data(),GL_DYNAMIC_DRAW);
    const GLuint reference=program(referenceVs(uid),gpu::genFragmentShader(uid),false);
    const auto expected=render(reference,nullptr,state);
    for (bool es100:{false,true}) {
        const auto dialect=es100?gpu::PortableDialect::Es100:gpu::PortableDialect::Es320Diagnostic;
        const GLuint portable=program(es100?gpu::genPortableCpuVertexShader(uid):referenceVs(uid),gpu::genPortableFragmentShader(uid,dialect),es100);
        const auto actual=render(portable,&layout,state);
        for (size_t i=0;i<actual.size();++i) {
            const float difference=std::abs(actual[i]-expected[i]); maximum=std::max(maximum,difference);
            const float tolerance=i<size_t(width*height*4)?0.000001f:0.0f;
            if (!std::isfinite(actual[i]) || !std::isfinite(expected[i]) || difference>tolerance) {
                fprintf(stderr,"variant=%zu es100=%d tex=%u stages=%u component=%zu expected=%g actual=%g\n",variants,es100,uid.numTexGens,uid.numTevStages,i,expected[i],actual[i]);
                for (uint32_t stage=0;stage<uid.numTevStages;++stage)
                    fprintf(stderr,"stage=%u cc=%06x ac=%06x\n",stage,uid.tevColor[stage],uid.tevAlpha[stage]);
                check(false,"integer/portable fragment mismatch");
            }
            ++compared;
        }
        glDeleteProgram(portable);
    }
    glDeleteProgram(reference); ++variants;
}

static void rasterFixture(gpu::ShaderUid uid) {
    gpu::PixelUniformLayout layout; std::string error;
    check(gpu::makePixelUniformLayout(uid,layout,error),"raster material layout");
    const auto state=pixelState(); const auto geometry=vertices();
    glBindBuffer(GL_ARRAY_BUFFER,vbo); glBufferData(GL_ARRAY_BUFFER,sizeof(geometry),geometry.data(),GL_DYNAMIC_DRAW);
    const float scissors[][8]={{3.5f,5.5f,31.5f,23.5f,1,0.625f,0,0},
                              {31,5,3,23,0,0,0,0},{0,0,64,64,0,0,0,0}};
    for (bool es100:{false,true}) {
        const auto dialect=es100?gpu::PortableDialect::Es100:gpu::PortableDialect::Es320Diagnostic;
        const auto vs=es100?gpu::genPortableCpuVertexShader(uid):referenceVs(uid);
        const GLuint legacy=program(vs,gpu::genPortableFragmentShader(uid,dialect),es100);
        const auto original=render(legacy,&layout,state);
        const GLuint raster=program(vs,gpu::genPortableRasterFragmentShader(uid,dialect),es100);
        for (const auto& bounds:scissors) {
            const auto actual=render(raster,&layout,state,bounds);
            for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
                const bool inside=x+0.5f>=bounds[0] && y+0.5f>=bounds[1] &&
                                  x+0.5f<bounds[2] && y+0.5f<bounds[3];
                const size_t pixel=size_t(y*width+x)*4;
                for (unsigned component=0;component<8;++component) {
                    const size_t offset=pixel+(component<4 ? component : size_t(width*height*4)+component-4);
                    float expected=inside ? original[offset] : component<4 ? -1.0f : 1.0f;
                    if (inside && component==3 && bounds[4]>0.5f && original[pixel+3]>=0) expected=bounds[5];
                    check(std::isfinite(actual[offset]) && std::abs(actual[offset]-expected)<=(component<4 ? 0.000001f : 0.0f),
                          "exact exclusive raster scissor, original alpha discard/depth, final alpha override");
                    ++compared;
                }
            }
        }
        glDeleteProgram(raster); glDeleteProgram(legacy);
    }
}

static void rgba6Fixture() {
    gpu::ShaderUid uid{}; uid.numTevStages=1; uid.alphaFunc=2 | 7<<3;
    uid.tevColor[0]=15<<12 | 15<<8 | 15<<4;
    uid.tevAlpha[0]=7<<13 | 7<<10 | 7<<7;
    gpu::PixelUniformLayout layout; std::string error;
    check(gpu::makePixelUniformLayout(uid,layout,error),"RGBA6 original byte-alpha comparison layout");
    auto geometry=vertices();
    for (auto& vertex:geometry) vertex.clip[2]=0;
    glBindBuffer(GL_ARRAY_BUFFER,vbo); glBufferData(GL_ARRAY_BUFFER,sizeof(geometry),geometry.data(),GL_DYNAMIC_DRAW);
    constexpr unsigned bayer[2][2]={{0,2},{3,1}};
    for (bool es100:{false,true}) {
        const auto dialect=es100?gpu::PortableDialect::Es100:gpu::PortableDialect::Es320Diagnostic;
        const auto vs=es100?gpu::genPortableCpuVertexShader(uid):referenceVs(uid);
        const GLuint shader=program(vs,gpu::genPortableRasterFragmentShader(uid,dialect),es100);
        for (unsigned dither:{0u,1u}) for (unsigned byte=0;byte<256;++byte) {
            const unsigned input[]={byte,255-byte,(byte+63)&255,(byte+127)&255};
            gpu::PixelState state{};
            for (unsigned c=0;c<4;++c) state.tevReg[0][c]=float(input[c])/255;
            state.alphaRef[0]=float(input[3]);
            const float raster[]={3,5,31,29,0,0,1,float(dither)};
            const auto actual=render(shader,&layout,state,raster);
            for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
                const bool inside=x>=3 && x<31 && y>=5 && y<29;
                const size_t pixel=size_t(y*width+x)*4;
                for (unsigned c=0;c<4;++c) {
                    unsigned color=input[c];
                    if (dither && c<3) color=color-(color>>6)+bayer[y&1][x&1];
                    const unsigned six=color>>2,expanded=(six<<2)|(six>>4);
                    const float expected=inside ? float(expanded)/255 : -1.0f;
                    if (std::abs(actual[pixel+c]-expected)>0.000001f)
                        fprintf(stderr,"RGBA6 dialect=%u dither=%u byte=%u xy=%d,%d c=%u expected=%.9g actual=%.9g\n",
                            unsigned(dialect),dither,byte,x,y,c,expected,actual[pixel+c]);
                    check(std::isfinite(actual[pixel+c]) && std::abs(actual[pixel+c]-expected)<=0.000001f,
                          "real RGBA6 byte truncation/expansion and EFB-coordinate RGB-only Bayer pattern after byte alpha test");
                    ++compared;
                }
            }
        }
        gpu::PixelState rejected{}; rejected.tevReg[0][3]=1.0f/255; rejected.alphaRef[0]=0;
        const float raster[]={0,0,32,32,0,0,1,1};
        const auto actual=render(shader,&layout,rejected,raster);
        for (size_t i=0;i<size_t(width*height*4);++i)
            check(actual[i]==-1,"RGBA6 alpha comparison must precede six-bit storage, including byte one versus zero");
        glDeleteProgram(shader);
    }
    puts("portable RGBA6: all 256 byte values, four Bayer positions, undithered alpha and pre-storage alpha tests; not native pixel proof");
}

static void destinationAlphaFixture() {
    gpu::ShaderUid uid{}; uid.numTevStages=1; uid.alphaFunc=2 | 7<<3; uid.efbHasAlpha=1;
    uid.tevColor[0]=15<<12 | 15<<8 | 15<<4;
    uid.tevAlpha[0]=7<<13 | 7<<10 | 7<<7;
    gpu::PixelUniformLayout layout; std::string error;
    check(gpu::makePixelUniformLayout(uid,layout,error),"destination-alpha byte-test layout");
    auto geometry=vertices();
    for (auto& vertex:geometry) vertex.clip[2]=0;
    glBindBuffer(GL_ARRAY_BUFFER,vbo); glBufferData(GL_ARRAY_BUFFER,sizeof(geometry),geometry.data(),GL_DYNAMIC_DRAW);
    constexpr unsigned bayer[2][2]={{0,2},{3,1}};
    constexpr unsigned destinations[][2]={{0,0},{3,0},{4,4},{63,60},{64,65},
                                         {127,125},{128,130},{191,190},{252,255},{255,255}};
    constexpr unsigned rgb[]={63,127,191};
    for (bool es100:{false,true}) {
        const auto dialect=es100?gpu::PortableDialect::Es100:gpu::PortableDialect::Es320Diagnostic;
        const auto vs=es100?gpu::genPortableCpuVertexShader(uid):referenceVs(uid);
        const GLuint shader=program(vs,gpu::genPortableRasterFragmentShader(uid,dialect),es100);
        for (unsigned dither:{0u,1u}) for (const auto& destination:destinations) {
            gpu::PixelState state{};
            for (unsigned c=0;c<3;++c) state.tevReg[0][c]=float(rgb[c])/255;
            state.tevReg[0][3]=1.0f/255; state.alphaRef[0]=1;
            const float raster[]={3,5,31,29,1,float(destination[1])/255,1,float(dither)};
            const auto actual=render(shader,&layout,state,raster);
            for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
                const bool inside=x>=3 && x<31 && y>=5 && y<29;
                const size_t pixel=size_t(y*width+x)*4;
                for (unsigned c=0;c<4;++c) {
                    unsigned stored=destination[1];
                    if (c<3) {
                        unsigned byte=rgb[c];
                        if (dither) byte=byte-(byte>>6)+bayer[y&1][x&1];
                        const unsigned six=byte>>2; stored=(six<<2)|(six>>4);
                    }
                    const float expected=inside ? float(stored)/255 : -1.0f;
                    if (std::abs(actual[pixel+c]-expected)>0.000001f)
                        fprintf(stderr,"destination-alpha dialect=%u dither=%u byte=%u xy=%d,%d c=%u expected=%.9g actual=%.9g\n",
                            unsigned(dialect),dither,destination[0],x,y,c,expected,actual[pixel+c]);
                    check(std::isfinite(actual[pixel+c]) && std::abs(actual[pixel+c]-expected)<=0.000001f,
                          "destination alpha follows original byte-alpha test and replaces only stored alpha after RGB dither");
                    check(actual[size_t(width*height*4)+pixel+c]==(inside ? 0.5f : 1.0f),
                          "destination-alpha scissor discards leave depth unchanged");
                    compared+=2;
                }
            }
        }
        gpu::PixelState rejected{}; rejected.tevReg[0][3]=1.0f/255; rejected.alphaRef[0]=0;
        const float raster[]={0,0,32,32,1,0,1,1};
        const auto actual=render(shader,&layout,rejected,raster);
        for (size_t i=0;i<size_t(width*height*4);++i) {
            check(actual[i]==-1 && actual[size_t(width*height*4)+i]==1,
                  "destination alpha cannot turn a rejected original alpha into color or an invisible depth occluder");
            compared+=2;
        }
        glDeleteProgram(shader);
    }
    puts("portable destination alpha: original byte-alpha tests, six-bit overrides, RGB-only dither, scissor and discard-depth composition; not native pixel proof");
}

int main() {
    context(); setup();
    depthEndpoints();
    rgba6Fixture();
    destinationAlphaFixture();
    for (int variant=0;variant<64;++variant) {
        gpu::ShaderUid uid{};
        uid.numTexGens=variant%9; uid.numChans=2; uid.numTevStages=1+variant%16;
        uid.numIndStages=variant%5; uid.alphaFunc=variant%3?63:generator()%256;
        uid.fogSel=variant%16; uid.zTex=variant%4?0:4+(variant%3);
        uid.iref=generator()&0xFFFFFF; uid.indScale[0]=generator()&0xFFFF; uid.indScale[1]=generator()&0xFFFF;
        for (uint32_t i=0;i<uid.numTevStages;++i) {
            uid.tevColor[i]=generator()&0xFFFFFF; uid.tevAlpha[i]=generator()&0xFFFFFF;
            uid.indCmd[i]=uint32_t(1+i%3)<<9 | uint32_t(i%8)<<4 | i%4;
        }
        for (int i=0;i<8;++i) { uid.tevOrder[i]=generator()&0xFFFFFF; uid.ksel[i]=generator()&0xFFFFFF; }
        fixture(uid);
    }
    // Every regular rounding/bias/scale/subtract mode, without clamp hiding it.
    for (uint32_t scale=0;scale<4;++scale) for (uint32_t bias=0;bias<3;++bias) for (uint32_t sub=0;sub<2;++sub) {
        gpu::ShaderUid uid{}; uid.numChans=2; uid.numTevStages=1; uid.alphaFunc=63;
        uid.tevOrder[0]=0; uid.ksel[0]=4 | 14<<4 | 25<<9;
        uid.tevColor[0]=2<<12 | 4<<8 | 14<<4 | 6 | bias<<16 | sub<<18 | scale<<20;
        uid.tevAlpha[0]=1<<13 | 2<<10 | 6<<7 | 3<<4 | bias<<16 | sub<<18 | scale<<20;
        fixture(uid);
    }
    gpu::ShaderUid maximal{};
    maximal.numChans=2; maximal.numTevStages=16; maximal.numTexGens=8; maximal.numIndStages=4;
    maximal.alphaFunc=63; maximal.fogSel=4; maximal.zTex=6;
    for (int i=0;i<4;++i) maximal.iref|=uint32_t(i|(i<<3))<<(i*6);
    for (int i=0;i<16;++i) {
        const uint32_t order=uint32_t(i%8)|uint32_t(i%8)<<3|64;
        maximal.tevOrder[i/2]|=order<<((i&1)*12);
        maximal.tevColor[i]=2<<12|4<<8|14<<4|6|uint32_t(i%3)<<16|uint32_t(i%2)<<18|uint32_t(i%4)<<20;
        maximal.tevAlpha[i]=1<<13|2<<10|6<<7|3<<4|uint32_t(i%3)<<16|uint32_t(i%2)<<18|uint32_t(i%4)<<20;
        maximal.ksel[i/2]|=uint32_t(12+i%4)<<((i&1)*10+4);
        maximal.indCmd[i]=uint32_t(1+i%3)<<9|uint32_t(i%8)<<4|i%4;
    }
    fixture(maximal);
    rasterFixture(maximal);
    maximal.alphaFunc=0; rasterFixture(maximal);
    gpu::ShaderUid invalid{}; gpu::PixelUniformLayout layout; std::string error;
    check(!gpu::makePixelUniformLayout(invalid,layout,error),"reject zero TEV stages");
    invalid.numTevStages=1; invalid.numTexGens=9;
    check(!gpu::makePixelUniformLayout(invalid,layout,error),"reject ninth texgen");
    check(maximumVectors==25,"maximal live uniform fixture");
    std::array<float,108> packed;
    layout.vectors=1; layout.slots.fill(-1); layout.slots[0]=0; layout.slots[1]=0;
    check(!gpu::packPixelUniforms(layout,{},packed),"reject duplicate uniform slot");
    layout.slots[1]=-2;
    check(!gpu::packPixelUniforms(layout,{},packed),"reject invalid negative slot");
    printf("portable TEV: %zu variants, %zu color/depth components, maximum color error %.9g; depth exact, both ES100 and ES320, maximum uniforms %u vec4\n",variants,compared,maximum,maximumVectors);
    return 0;
}
