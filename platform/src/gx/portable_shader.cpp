#include "portable_shader.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace gpu {
namespace {

struct Output {
    std::string text;
    void write(const char* format, ...) __attribute__((format(printf, 2, 3))) {
        va_list arguments, count;
        va_start(arguments, format);
        va_copy(count, arguments);
        const int size = vsnprintf(nullptr, 0, format, count);
        va_end(count);
        if (size >= 0) {
            const size_t start = text.size();
            text.resize(start + size + 1);
            vsnprintf(text.data() + start, size + 1, format, arguments);
            text.resize(start + size);
        }
        va_end(arguments);
    }
};

bool validUid(const ShaderUid& u) {
    return u.numTexGens <= 8 && u.numChans <= 2 && u.numIndStages <= 4 &&
        u.numTevStages >= 1 && u.numTevStages <= 16 && !u.cutaway && !u.screenTexGens;
}

const char* colorInput[16] = {"prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa", "c2.rgb", "c2.aaa",
    "tex.rgb", "tex.aaa", "ras.rgb", "ras.aaa", "vec3(255.0)", "vec3(128.0)", "kc", "vec3(0.0)"};
const char* alphaInput[8] = {"prev.a", "c0.a", "c1.a", "c2.a", "tex.a", "ras.a", "ka", "0.0"};
const char* destinations[4] = {"prev", "c0", "c1", "c2"};
const char components[] = "rgba";
const int fractions[8] = {255, 223, 191, 159, 128, 96, 64, 32};

std::string swap(const ShaderUid& u, uint32_t table) {
    const uint32_t lo = u.ksel[table * 2], hi = u.ksel[table * 2 + 1];
    std::string result = ".";
    result += components[lo & 3]; result += components[(lo >> 2) & 3];
    result += components[hi & 3]; result += components[(hi >> 2) & 3];
    return result;
}

std::string konst(uint32_t selection, bool alpha) {
    if (selection < 8) {
        const std::string value = std::to_string(fractions[selection]) + ".0";
        return alpha ? value : "vec3(" + value + ")";
    }
    if (!alpha && selection >= 12 && selection < 16)
        return "konst" + std::to_string(selection - 12) + ".rgb";
    if (selection >= 16) {
        const std::string scalar = "konst" + std::to_string((selection - 16) & 3) + "." + components[(selection - 16) >> 2];
        return alpha ? scalar : "vec3(" + scalar + ")";
    }
    return alpha ? "0.0" : "vec3(0.0)";
}

std::string comparison(uint32_t mode, bool alpha) {
    const char* result = alpha ? "tevin_c.a" : "tevin_c.rgb";
    const char* zero = alpha ? "0.0" : "vec3(0.0)";
    std::string a = "tevin_a.r", b = "tevin_b.r";
    if (mode / 2 == 1 || mode / 2 == 2) {
        const char* weights = mode / 2 == 1 ? "vec3(1.0,256.0,0.0)" : "vec3(1.0,256.0,65536.0)";
        a = "gxDot(tevin_a.rgb," + std::string(weights) + ")";
        b = "gxDot(tevin_b.rgb," + std::string(weights) + ")";
    } else if (mode / 2 == 3) {
        if (alpha) { a = "tevin_a.a"; b = "tevin_b.a"; }
        else if (mode & 1)
            return "vec3((tevin_a.r==tevin_b.r)?tevin_c.r:0.0,(tevin_a.g==tevin_b.g)?tevin_c.g:0.0,(tevin_a.b==tevin_b.b)?tevin_c.b:0.0)";
        else
            return "vec3((tevin_a.r>tevin_b.r)?tevin_c.r:0.0,(tevin_a.g>tevin_b.g)?tevin_c.g:0.0,(tevin_a.b>tevin_b.b)?tevin_c.b:0.0)";
    }
    return "((" + a + ((mode & 1) ? "==" : ">") + b + ")?" + result + ":" + zero + ")";
}

std::string regular(uint32_t word, bool alpha) {
    const uint32_t bias = (word >> 16) & 3, sub = (word >> 18) & 1, scale = (word >> 20) & 3;
    const char* c = alpha ? "a" : "rgb";
    const char* biases[] = {"", "+128.0", "-128.0"};
    const char* scales[] = {"1.0", "2.0", "4.0", "1.0"};
    // All integer-valued intermediates here are below 2^20. floor is the
    // signed arithmetic shift, including negative D and subtract rounding.
    const bool rounded = (scale == 3) == alpha;
    const char* roundBias = rounded ? (sub ? "+127.0" : "+128.0") : "";
    Output out;
    out.write("((tevin_d.%s%s)*%s %c floor(((tevin_a.%s*256.0+(tevin_b.%s-tevin_a.%s)*(tevin_c.%s+floor(tevin_c.%s/128.0)))*%s%s)/256.0))",
        c, biases[bias], scales[scale], sub ? '-' : '+', c, c, c, c, c, scales[scale], roundBias);
    return scale == 3 ? "floor(" + out.text + "/2.0)" : out.text;
}

std::string alphaTest(uint32_t function, const char* reference) {
    const char* operators[] = {nullptr, "<", "==", "<=", ">", "!=", ">="};
    if (!function) return "false";
    if (function == 7) return "true";
    return "(a" + std::string(operators[function]) + reference + ")";
}

void varyingDeclarations(Output& out, const ShaderUid& u, bool es100, bool vertex) {
    const char* qualifier = es100 ? "varying" : vertex ? "out" : "in";
    out.write("%s vec4 v_clr0;\n%s vec4 v_clr1;\n", qualifier, qualifier);
    for (uint32_t i = 0; i < u.numTexGens; ++i)
        out.write("%s %s v_tex%u;\n", qualifier, es100 ? "vec4" : "vec3", i);
    if (!es100) out.write("%s float v_viewz;\n%s float v_gxz;\n", qualifier, qualifier);
    else if (u.numTexGens < 2) out.write("varying vec2 v_depth;\n");
}

void cgVaryings(Output& out, const ShaderUid& u, bool vertex) {
    out.write("struct Varyings {\n");
    if (vertex) out.write("float4 position : POSITION;\n");
    out.write("float4 v_clr0 : COLOR0;\nfloat4 v_clr1 : COLOR1;\n");
    for (uint32_t i = 0; i < u.numTexGens; ++i)
        out.write("float4 v_tex%u : TEXCOORD%u;\n", i, i);
    if (u.numTexGens < 2) out.write("float2 v_depth : TEXCOORD%u;\n", u.numTexGens);
    out.write("};\n");
}

} // namespace

bool makePixelUniformLayout(const ShaderUid& u, PixelUniformLayout& layout, std::string& error) {
    layout = {};
    layout.slots.fill(-1);
    error.clear();
    if (!validUid(u)) { error = "portable shader: malformed counts or VR-only state"; return false; }
    std::array<bool, 27> used{};
    // Preserve all register entry values; later stages can read a preceding
    // stage's destination. No assumptions about material-specific liveness.
    for (int i = 0; i < 4; ++i) used[i] = true;
    const bool alpha = (u.alphaFunc & 63) != 63;
    used[8] = alpha || u.zTex;
    const uint32_t fog = (u.fogSel >> 1) & 7;
    used[9] = used[10] = fog >= 2;
    for (uint32_t i = 0; i < u.numIndStages; ++i) {
        const uint32_t map = (u.iref >> (i * 6)) & 7, coord = (u.iref >> (i * 6 + 3)) & 7;
        if (coord < u.numTexGens) { used[12 + map] = true; layout.textureMask |= 1u << map; }
    }
    for (uint32_t i = 0; i < u.numTevStages; ++i) {
        const uint32_t selection = u.ksel[i / 2] >> ((i & 1) * 10);
        const uint32_t kc = (selection >> 4) & 31, ka = (selection >> 9) & 31;
        if (kc >= 12) used[4 + (kc >= 16 ? (kc - 16) & 3 : kc - 12)] = true;
        if (ka >= 16) used[4 + ((ka - 16) & 3)] = true;
        const uint32_t order = (u.tevOrder[i / 2] >> ((i & 1) * 12)) & 0xFFF;
        const uint32_t map = order & 7;
        if (order & 64) { used[12 + map] = true; layout.textureMask |= 1u << map; }
        const uint32_t indirect = u.indCmd[i], stage = indirect & 3, matrix = (indirect >> 9) & 15;
        if (u.numIndStages && indirect && stage < u.numIndStages && (matrix & 3)) {
            const uint32_t row = 20 + ((matrix & 3) - 1) * 2;
            used[row] = used[row + 1] = used[12 + map] = true;
        }
    }
    for (size_t i = 0; i < used.size(); ++i) if (used[i]) layout.slots[i] = layout.vectors++;
    return true;
}

bool describeVitaFragment(const ShaderUid& uid,VitaFragmentProfile profile,VitaFragmentLayout& out,std::string& error,uint32_t manualMask) {
    out={};
    VitaFragmentLayout layout;
    if (!makePixelUniformLayout(uid,layout.pixel,error)) return false;
    if (!isKnownVitaFragmentProfile(profile)) {
        error="unknown native fragment source profile"; return false;
    }
    if (manualMask&~layout.pixel.textureMask) { error="manual sampling requires original UID-used texture units"; return false; }
    if (manualMask) for(unsigned stage=0;stage<uid.numTevStages;++stage) {
        if (uid.numIndStages && (uid.indCmd[stage]&~0x673u)) {
            error="manual sampling needs a verified eight-bit full-matrix post-indirect coordinate contract"; return false;
        }
    }
    if (profile!=VitaFragmentProfile::Raster) {
        if (((uid.fogSel>>1)&7)>=2) { error="snapshot profile has no verified fog strategy"; return false; }
        if (layout.pixel.vectors>=27) { error="snapshot uniform exceeds native 27-vector capacity"; return false; }
        for (uint32_t sampler=0;sampler<8;++sampler) if (!(layout.pixel.textureMask&(1u<<sampler))) {
            layout.snapshotSampler=sampler; layout.snapshotSlot=layout.pixel.vectors++;
            layout.pixel.textureMask|=1u<<sampler; break;
        }
        if(layout.snapshotSampler==8) { error="snapshot profile needs an unoccupied original texture unit"; return false; }
    }
    layout.manualTextureMask=manualMask;
    for(unsigned unit=0;unit<8;++unit) if(manualMask&(1u<<unit)) {
        if(layout.pixel.vectors>=27) { error="manual sampler uniform exceeds native 27-vector capacity"; return false; }
        layout.manualSlots[unit]=int(layout.pixel.vectors++);
    }
    out=layout; return true;
}

bool packPixelUniforms(const PixelUniformLayout& layout, const PixelState& state, std::array<float, 108>& output) {
    static_assert(sizeof(PixelState) == 27 * 16);
    output.fill(0);
    if (layout.vectors > 27) return false;
    std::array<bool, 27> filled{};
    const auto* bytes = reinterpret_cast<const unsigned char*>(&state);
    for (size_t i = 0; i < layout.slots.size(); ++i) {
        const int slot = layout.slots[i];
        if (slot == -1) continue;
        if (slot < 0 || uint32_t(slot) >= layout.vectors || filled[slot]) { output.fill(0); return false; }
        filled[slot] = true;
        memcpy(output.data() + slot * 4, bytes + i * 16, 16);
    }
    for (uint32_t i = 0; i < layout.vectors; ++i) if (!filled[i]) { output.fill(0); return false; }
    return true;
}

bool packVitaFragmentUniforms(const ShaderUid& uid,VitaFragmentProfile profile,const PixelState& state,
    uint32_t width,uint32_t height,bool alphaOverride,uint32_t alphaByte,std::array<float,108>& output,std::string& error,uint32_t manualMask) {
    output.fill(0);
    VitaFragmentLayout extended; PixelUniformLayout base;
    if (!describeVitaFragment(uid,profile,extended,error,manualMask) || !makePixelUniformLayout(uid,base,error)) return false;
    if (profile!=VitaFragmentProfile::Raster &&
        (!width || !height || width>4096 || height>4096 || alphaByte>255)) {
        error="invalid native snapshot extent or destination alpha byte"; return false;
    }
    if (!packPixelUniforms(base,state,output)) { error="invalid original compact pixel payload"; return false; }
    if (profile!=VitaFragmentProfile::Raster) {
        const size_t at=extended.snapshotSlot*4;
        output[at]=float(width); output[at+1]=float(height);
        output[at+2]=alphaOverride ? 1.0f : 0.0f; output[at+3]=float(alphaByte);
    }
    return true;
}

std::string genPortableCpuVertexShader(const ShaderUid& u) {
    if (!validUid(u)) return {};
    Output out;
    out.write("#version 100\nprecision highp float;\nattribute vec4 a_clip;\nattribute vec4 a_clr0;\nattribute vec4 a_clr1;\nattribute vec2 a_depth;\n");
    for (uint32_t i = 0; i < u.numTexGens; ++i) out.write("attribute vec3 a_tex%u;\n", i);
    varyingDeclarations(out, u, true, true);
    out.write("void main(){\ngl_Position=a_clip;\nv_clr0=a_clr0;\nv_clr1=a_clr1;\n");
    for (uint32_t i = 0; i < u.numTexGens; ++i)
        out.write("v_tex%u=vec4(a_tex%u,%s);\n", i, i, i == 0 ? "a_depth.x" : i == 1 ? "a_depth.y" : "0.0");
    if (u.numTexGens < 2) out.write("v_depth=a_depth;\n");
    out.write("}\n");
    return out.text;
}

std::string genVitaCpuVertexShader(const ShaderUid& u) {
    if (!validUid(u)) return {};
    Output out;
    cgVaryings(out, u, true);
    out.write("Varyings main(float4 a_clip, float4 a_clr0, float4 a_clr1, float2 a_depth");
    for (uint32_t i = 0; i < u.numTexGens; ++i) out.write(", float3 a_tex%u", i);
    out.write("){\nVaryings result;\nresult.position=a_clip;\n");
    out.write("result.v_clr0=a_clr0;\nresult.v_clr1=a_clr1;\n");
    for (uint32_t i = 0; i < u.numTexGens; ++i)
        out.write("result.v_tex%u=float4(a_tex%u,%s);\n", i, i, i == 0 ? "a_depth.x" : i == 1 ? "a_depth.y" : "0.0");
    if (u.numTexGens < 2) out.write("result.v_depth=a_depth;\n");
    out.write("return result;\n}\n");
    return out.text;
}

static std::string genFragmentShader(const ShaderUid& u, PortableDialect dialect, bool raster,int snapshotSampler=-1,
    bool additiveSnapshot=false,const VitaFragmentLayout* manualLayout=nullptr,bool srcAlphaDitherSnapshot=false,
    bool srcAlphaAddSnapshot=false) {
    PixelUniformLayout layout;
    std::string error;
    if (!makePixelUniformLayout(u, layout, error)) return {};
    const bool cg = dialect == PortableDialect::VitaCg;
    const bool es100 = dialect == PortableDialect::Es100 || cg;
    Output out;
    if (cg) {
        out.write("typedef float2 vec2;\ntypedef float3 vec3;\ntypedef float4 vec4;\n");
        out.write("vec3 mix(vec3 a,vec3 b,float t){return lerp(a,b,t);}\n");
        out.write("vec4 gxSample(sampler2D s,vec2 uv,float bias){return tex2Dbias(s,vec4(uv,0.0,bias));}\n");
        cgVaryings(out, u, false);
        out.write("struct FragmentOutput {\nfloat4 color : COLOR0;\n");
        if (u.zTex) out.write("float depth : DEPTH;\n");
        out.write("};\n");
    } else {
        out.write("#version %s\n", es100 ? "100" : "320 es");
        if (u.zTex && es100) out.write("#extension GL_EXT_frag_depth : require\n");
        out.write("precision highp float;\n");
        if (!es100) out.write("out vec4 o_color;\n");
        varyingDeclarations(out, u, es100, false);
    }
    out.write("uniform vec4 u_pixel[%u];\n", manualLayout ? manualLayout->pixel.vectors : layout.vectors+unsigned(snapshotSampler>=0));
    if (raster) out.write("uniform vec4 u_raster[2];\n");
    for (int i = 0; i < 8; ++i) if ((layout.textureMask & (1u << i)) || i==snapshotSampler) {
        if (cg) out.write("uniform sampler2D s_tex%d : TEXUNIT%d;\n", i, i);
        else out.write("uniform sampler2D s_tex%d;\n", i);
    }
    out.write("vec2 projUv(vec3 t){return (t.z!=0.0)?t.xy/t.z:t.xy;}\n");
    if(manualLayout && manualLayout->manualTextureMask) out.write(R"CG(
// Provisional pinned GPU reference: original 17.7 coordinates and byte filtering.
vec2 gxSignedTrunc(vec2 x){return sign(x)*floor(abs(x));}
float gxWrapCorner(float x,float size,float mode){
 if(mode<0.5)return clamp(x,0.0,size-1.0);
 float period=mode<1.5?size:size*2.0;
 float p=x-period*floor(x/period);
 return mode>1.5&&p>=size?period-1.0-p:p;
}
vec4 gxPointByte(sampler2D s,vec2 p,float size,float level,vec2 wrap){
 p=vec2(gxWrapCorner(p.x,size,wrap.x),gxWrapCorner(p.y,size,wrap.y));
 return floor(tex2Dlod(s,vec4((p+0.5)/size,0.0,level))*255.0+0.5);
}
vec4 gxLinearByte(sampler2D s,vec2 fixedUv,float level,float size,vec2 wrap){
 vec2 shifted=floor(fixedUv/exp2(level))-64.0;
 vec2 corner=floor(shifted/128.0),fraction=shifted-corner*128.0;
 float mipSize=max(1.0,floor(size/exp2(level)));
 vec4 a=gxPointByte(s,corner,mipSize,level,wrap);
 vec4 b=gxPointByte(s,corner+vec2(1.0,0.0),mipSize,level,wrap);
 vec4 c=gxPointByte(s,corner+vec2(0.0,1.0),mipSize,level,wrap);
 vec4 d=gxPointByte(s,corner+vec2(1.0,1.0),mipSize,level,wrap);
 return floor(((a*(128.0-fraction.x)+b*fraction.x)*(128.0-fraction.y)+
               (c*(128.0-fraction.x)+d*fraction.x)*fraction.y)/16384.0);
}
vec4 gxManualSample(sampler2D s,vec2 uv,vec4 texSize,vec4 p){
 vec2 fixedUv=gxSignedTrunc(uv/texSize.zw*128.0);
 vec2 delta=abs(ddx(fixedUv))+abs(ddy(fixedUv));
 float magnitude=max(delta.x,delta.y);
 float selected=floor((log2(max(magnitude,1.0))-7.0)*16.0)+floor(texSize.x*16.0);
 float lod=lerp(p.x,selected,step(1.0,magnitude));
 lod=clamp(clamp(lod,p.x,p.y),0.0,p.z*16.0);
 float level=floor(lod/16.0),fraction=lod-level*16.0;
 vec2 wrap=vec2(p.w-4.0*floor(p.w/4.0),floor(p.w/4.0));
 vec4 bytes=gxLinearByte(s,fixedUv,level,1.0/texSize.z,wrap);
 if(fraction>0.0&&level<p.z){
  vec4 next=gxLinearByte(s,fixedUv,level+1.0,1.0/texSize.z,wrap);
  bytes=floor((bytes*(16.0-fraction)+next*fraction)/16.0);
 }
 return bytes/255.0;
}
)CG");
    out.write("vec4 gxByte(vec4 x){return x-256.0*floor(x/256.0);}\n");
    out.write("float gxRound(float x){float lo=floor(x);float fraction=x-lo;if(fraction>0.5)return lo+1.0;if(fraction<0.5)return lo;return (lo-2.0*floor(lo/2.0)==0.0)?lo:lo+1.0;}\n");
    out.write("vec4 gxRound(vec4 x){return vec4(gxRound(x.x),gxRound(x.y),gxRound(x.z),gxRound(x.w));}\n");
    out.write("float gxDot(vec3 a,vec3 b){vec3 t=a*b;return (t.x+t.y)+t.z;}\n");
    if (((u.zTex >> 2) & 3) == 1) out.text += kZ24WindowDepthGlsl;
    if (cg) {
        out.write("FragmentOutput main(Varyings input,float4 gl_FragCoord : WPOS){\nFragmentOutput result;\n");
        out.write("vec4 v_clr0=input.v_clr0,v_clr1=input.v_clr1;\n");
        for (uint32_t i = 0; i < u.numTexGens; ++i) out.write("vec4 v_tex%u=input.v_tex%u;\n", i, i);
        if (u.numTexGens < 2) out.write("vec2 v_depth=input.v_depth;\n");
    } else out.write("void main(){\n");
    auto uniform = [&](const char* name, int source) {
        if (layout.slots[source] < 0) out.write("vec4 %s=vec4(0.0);\n", name);
        else out.write("vec4 %s=u_pixel[%d];\n", name, layout.slots[source]);
    };
    for (int i = 0; i < 4; ++i) {
        out.write("vec4 %s=gxRound(u_pixel[%d]*255.0);\n", destinations[i], layout.slots[i]);
        const std::string name = "konst" + std::to_string(i);
        uniform(name.c_str(), 4 + i);
        out.write("%s=gxRound(%s*255.0);\n", name.c_str(), name.c_str());
    }
    uniform("alphaRef", 8); uniform("fogColor", 9); uniform("fogParams", 10);
    for (int i = 0; i < 8; ++i) uniform(("texSize" + std::to_string(i)).c_str(), 12 + i);
    for (int i = 0; i < 6; ++i) uniform(("indMtx" + std::to_string(i)).c_str(), 20 + i);
    out.write("vec4 tex=vec4(0.0),ras=vec4(0.0),tevin_a,tevin_b,tevin_c,tevin_d;\nvec3 kc=vec3(0.0);\nfloat ka=0.0;\n");
    if (u.zTex) out.write("vec4 ztex=vec4(0.0);\n");
    const char* sample = cg ? "gxSample" : es100 ? "texture2D" : "texture";
    auto sampled=[&](uint32_t map,const std::string& uv) {
        Output expression;
        if(manualLayout && (manualLayout->manualTextureMask&(1u<<map)))
            expression.write("gxManualSample(s_tex%u,%s,texSize%u,u_pixel[%d])",map,uv.c_str(),map,manualLayout->manualSlots[map]);
        else expression.write("%s(s_tex%u,%s,texSize%u.x)",sample,map,uv.c_str(),map);
        return expression.text;
    };
    for (uint32_t i = 0; i < u.numIndStages; ++i) {
        const uint32_t map = (u.iref >> (i * 6)) & 7, coord = (u.iref >> (i * 6 + 3)) & 7;
        const uint32_t scale = (u.indScale[i / 2] >> ((i & 1) * 8)) & 255;
        if (coord < u.numTexGens) {
            Output uv; uv.write("projUv(v_tex%u.xyz)/vec2(%.1f,%.1f)",coord,double(1u << (scale & 15)),double(1u << (scale >> 4)));
            out.write("vec3 ind%u=floor(%s.abg*255.0+0.5);\n",i,sampled(map,uv.text).c_str());
        }
        else out.write("vec3 ind%u=vec3(0.0);\n", i);
    }
    uint32_t lastColor = 0, lastAlpha = 0;
    for (uint32_t i = 0; i < u.numTevStages; ++i) {
        const uint32_t order = (u.tevOrder[i / 2] >> ((i & 1) * 12)) & 0xFFF;
        const uint32_t map = order & 7, coord = (order >> 3) & 7, channel = (order >> 7) & 7;
        const uint32_t cc = u.tevColor[i], ac = u.tevAlpha[i];
        const uint32_t selection = u.ksel[i / 2] >> ((i & 1) * 10);
        out.write("// stage %u\n", i);
        std::string uv = coord < u.numTexGens ? "projUv(v_tex" + std::to_string(coord) + ".xyz)" : "vec2(0.0)";
        const uint32_t indirect = u.indCmd[i], stage = indirect & 3, matrix = (indirect >> 9) & 15, bias = (indirect >> 4) & 7;
        if (u.numIndStages && indirect && stage < u.numIndStages && (matrix & 3)) {
            Output offset;
            offset.write("(%s+vec2(dot(indMtx%u.xyz,ind%u-vec3(%s,%s,%s)),dot(indMtx%u.xyz,ind%u-vec3(%s,%s,%s)))*texSize%u.zw)",
                uv.c_str(), ((matrix & 3) - 1) * 2, stage, bias & 1 ? "128.0" : "0.0", bias & 2 ? "128.0" : "0.0", bias & 4 ? "128.0" : "0.0",
                ((matrix & 3) - 1) * 2 + 1, stage, bias & 1 ? "128.0" : "0.0", bias & 2 ? "128.0" : "0.0", bias & 4 ? "128.0" : "0.0", map);
            uv = std::move(offset.text);
        }
        if (order & 64) {
            out.write("tex=gxRound(%s%s*255.0);\n",sampled(map,uv).c_str(),swap(u,(ac>>2)&3).c_str());
            if (u.zTex) out.write("ztex=tex;\n");
        } else out.write("tex=vec4(0.0);\n");
        out.write("ras=gxRound(clamp(%s,0.0,1.0)%s*255.0);\n", channel == 0 ? "v_clr0" : channel == 1 ? "v_clr1" : "vec4(0.0)", swap(u, ac & 3).c_str());
        out.write("kc=%s;\nka=%s;\n", konst((selection >> 4) & 31, false).c_str(), konst((selection >> 9) & 31, true).c_str());
        out.write("tevin_a=gxByte(vec4(%s,%s));\ntevin_b=gxByte(vec4(%s,%s));\ntevin_c=gxByte(vec4(%s,%s));\ntevin_d=vec4(%s,%s);\n",
            colorInput[(cc >> 12) & 15], alphaInput[(ac >> 13) & 7], colorInput[(cc >> 8) & 15], alphaInput[(ac >> 10) & 7],
            colorInput[(cc >> 4) & 15], alphaInput[(ac >> 7) & 7], colorInput[cc & 15], alphaInput[(ac >> 4) & 7]);
        for (int component = 0; component < 2; ++component) {
            const bool alpha = component == 1;
            const uint32_t word = alpha ? ac : cc, destination = (word >> 22) & 3;
            const bool clamp = (word >> 19) & 1;
            const std::string expression = ((word >> 16) & 3) != 3 ? regular(word, alpha) :
                std::string(alpha ? "tevin_d.a+" : "tevin_d.rgb+") + comparison((((word >> 20) & 3) << 1) | ((word >> 18) & 1), alpha);
            out.write("%s.%s=clamp(%s,%s,%s);\n", destinations[destination], alpha ? "a" : "rgb", expression.c_str(),
                alpha ? (clamp ? "0.0" : "-1024.0") : (clamp ? "vec3(0.0)" : "vec3(-1024.0)"),
                alpha ? (clamp ? "255.0" : "1023.0") : (clamp ? "vec3(255.0)" : "vec3(1023.0)"));
            if (alpha) lastAlpha = destination; else lastColor = destination;
        }
    }
    out.write("vec4 outi=gxByte(vec4(%s.rgb,%s.a));\nvec4 outc=outi/255.0;\n", destinations[lastColor], destinations[lastAlpha]);
    const uint32_t f0 = u.alphaFunc & 7, f1 = (u.alphaFunc >> 3) & 7, logic = (u.alphaFunc >> 6) & 3;
    if (!(f0 == 7 && f1 == 7)) {
        const char* operators[] = {"&&", "||", "!=", "=="};
        out.write("float a=outi.a;\nif(!((%s)%s(%s)))discard;\n", alphaTest(f0, "alphaRef.x").c_str(), operators[logic], alphaTest(f1, "alphaRef.y").c_str());
    }
    if (u.zTex) {
        out.write("{\nvec3 zb=vec3(0.0);\n");
        const uint32_t type = u.zTex & 3, op = (u.zTex >> 2) & 3;
        if (!type) out.write("zb.x=ztex.a;\n");
        else if (type == 1) out.write("zb.xy=ztex.ra;\n");
        else out.write("zb=ztex.bgr;\n");
        // Byte/carry arithmetic prevents >24-bit sums from losing low bits
        // on the Vita float ALU. Bias/current Z are individually exact F32.
        out.write("float bias=floor(alphaRef.z);\nzb+=vec3(bias-256.0*floor(bias/256.0),floor(bias/256.0)-256.0*floor(bias/65536.0),floor(bias/65536.0));\n");
        if (op == 1) out.write("%sfloat fragmentZ=gl_FragCoord.z;\nfloat current=gxWindowDepth(fragmentZ);\nzb+=vec3(current-256.0*floor(current/256.0),floor(current/256.0)-256.0*floor(current/65536.0),floor(current/65536.0));\n", cg ? "" : "highp ");
        out.write("zb.y+=floor(zb.x/256.0);\nzb.x-=256.0*floor(zb.x/256.0);\nzb.z+=floor(zb.y/256.0);\nzb.y-=256.0*floor(zb.y/256.0);\nzb.z-=256.0*floor(zb.z/256.0);\n%s=(zb.x+zb.y*256.0+zb.z*65536.0)/16777215.0;\n}\n", cg ? "result.depth" : es100 ? "gl_FragDepthEXT" : "gl_FragDepth");
    }
    const uint32_t fog = (u.fogSel >> 1) & 7;
    if (fog >= 2) {
        const char* gxz = es100 ? (u.numTexGens >= 2 ? "v_tex1.w" : "v_depth.y") : "v_gxz";
        out.write("{\nfloat zc=floor(clamp(%s,0.0,1.0)*16777215.0);\n", gxz);
        if (u.fogSel & 1) out.write("float ze=fogParams.x*zc/16777216.0;\n");
        else out.write("float ze=(fogParams.x*16777216.0)/(fogParams.z-floor(zc/exp2(fogParams.w)));\n");
        out.write("float fz=clamp(ze-fogParams.y,0.0,1.0);\n");
        if (fog == 4) out.write("fz=1.0-exp2(-8.0*fz);\n");
        if (fog == 5) out.write("fz=1.0-exp2(-8.0*fz*fz);\n");
        if (fog == 6) out.write("fz=exp2(-8.0*(1.0-fz));\n");
        if (fog == 7) out.write("fz=exp2(-8.0*(1.0-fz)*(1.0-fz));\n");
        out.write("outc.rgb=mix(outc.rgb,fogColor.rgb,fz);\n}\n");
    }
    // Sample before the exact pixel scissor so implicit texture derivatives
    // remain unchanged even at a scissor boundary within a GXM tile.
    if (raster) {
        out.write("if(gl_FragCoord.x<u_raster[0].x||gl_FragCoord.y<u_raster[0].y||gl_FragCoord.x>=u_raster[0].z||gl_FragCoord.y>=u_raster[0].w)discard;\n");
        if (snapshotSampler>=0) {
            // Integer byte-factor sums, including saturated weighted addition, are exact F32.
            // Original alpha discard/depth precede blend; override follows factors.
            out.write("vec4 snapshotDestination=floor(tex2D(s_tex%d,gl_FragCoord.xy/u_pixel[%u].xy)*255.0+0.5);\n",snapshotSampler,layout.vectors);
            if (additiveSnapshot || srcAlphaAddSnapshot) {
                // Provisional software-EFB ordering: clamp byte sum before RGB dither.
                if (srcAlphaAddSnapshot) {
                    out.write("float snapshotSrc=outi.a+floor(outi.a/128.0);\n");
                    out.write("vec4 stored=min(floor((outi*snapshotSrc+snapshotDestination*256.0)/256.0),vec4(255.0));\n");
                } else out.write("vec4 stored=min(outi+snapshotDestination,vec4(255.0));\n");
                out.write("if(u_raster[1].w>0.5){\nvec2 p=floor(gl_FragCoord.xy);\np-=2.0*floor(p/2.0);\nfloat bayer=2.0*abs(p.x-p.y)+p.y;\nstored.rgb+=vec3(bayer)-floor(stored.rgb/64.0);\n}\n");
            } else {
                out.write("float snapshotSrc=outi.a+floor(outi.a/128.0);\nfloat snapshotInverse=255.0-outi.a;\nfloat snapshotDst=snapshotInverse+floor(snapshotInverse/128.0);\n");
                out.write("vec4 stored=floor((outi*snapshotSrc+snapshotDestination*snapshotDst)/256.0);\n");
                if (srcAlphaDitherSnapshot) {
                    // This immutable profile is dither-on; preserve the legacy undithered source.
                    out.write("{\nvec2 p=floor(gl_FragCoord.xy);\np-=2.0*floor(p/2.0);\nfloat bayer=2.0*abs(p.x-p.y)+p.y;\nstored.rgb+=vec3(bayer)-floor(stored.rgb/64.0);\n}\n");
                }
            }
            out.write("if(u_pixel[%u].z>0.5)stored.a=floor(u_pixel[%u].w);\nstored=floor(stored/4.0);\noutc=(stored*4.0+floor(stored/16.0))/255.0;\n",layout.vectors,layout.vectors);
        } else {
            // No-fog/unblended RGBA6 stores six-bit bytes in the RGBA8 EFB.
            out.write("if(u_raster[1].z>0.5){\nvec4 stored=outi;\n");
            out.write("if(u_raster[1].w>0.5){\nvec2 p=floor(gl_FragCoord.xy);\np-=2.0*floor(p/2.0);\nfloat bayer=2.0*abs(p.x-p.y)+p.y;\nstored.rgb+=vec3(bayer)-floor(stored.rgb/64.0);\n}\n");
            out.write("stored=floor(stored/4.0);\noutc=(stored*4.0+floor(stored/16.0))/255.0;\n}\n");
            out.write("if(u_raster[1].x>0.5)outc.a=u_raster[1].y;\n");
        }
    }
    out.write("%s=clamp(outc,0.0,1.0);\n", cg ? "result.color" : es100 ? "gl_FragColor" : "o_color");
    if (cg) out.write("return result;\n");
    out.write("}\n");
    return out.text;
}

std::string genPortableFragmentShader(const ShaderUid& u, PortableDialect dialect) {
    return genFragmentShader(u, dialect, false);
}

std::string genPortableRasterFragmentShader(const ShaderUid& u, PortableDialect dialect) {
    return genFragmentShader(u, dialect, true);
}

std::string genVitaFragmentShader(const ShaderUid& uid,VitaFragmentProfile profile,uint32_t manualMask) {
    if (profile==VitaFragmentProfile::Raster && !manualMask) return genPortableRasterFragmentShader(uid,PortableDialect::VitaCg);
    VitaFragmentLayout layout; std::string error;
    if (!describeVitaFragment(uid,profile,layout,error,manualMask)) return {};
    return genFragmentShader(uid,PortableDialect::VitaCg,true,
        layout.snapshotSampler<8 ? int(layout.snapshotSampler) : -1,profile==VitaFragmentProfile::Rgba6AddSnapshot,
        manualMask ? &layout : nullptr,profile==VitaFragmentProfile::Rgba6SrcAlphaDitherSnapshot,
        profile==VitaFragmentProfile::Rgba6SrcAlphaAddSnapshot);
}

} // namespace gpu
