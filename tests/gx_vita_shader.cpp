// Generated Cg contract checks; compiler acceptance is a separate toolchain gate.
#include "portable_shader.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace gpu {
std::string genPortableRasterFragmentShader(const ShaderUid&, PortableDialect) __attribute__((weak));
bool describeVitaFragment(const ShaderUid&,VitaFragmentProfile,VitaFragmentLayout&,std::string&,uint32_t) __attribute__((weak));
std::string genVitaFragmentShader(const ShaderUid&,VitaFragmentProfile,uint32_t) __attribute__((weak));
}

static void require(bool condition, const char* message) {
    if (!condition) { fprintf(stderr, "Vita shader FAILED: %s\n", message); exit(1); }
}

int main(int argc, char** argv) {
    const std::filesystem::path output = argc > 1 ? argv[1] : "build-gx-vita-shader";
    std::filesystem::create_directories(output);
    gpu::ShaderUid wide{};
    for (unsigned texgens = 0; texgens <= 8; ++texgens) {
        gpu::ShaderUid uid{};
        uid.numTexGens = texgens; uid.numChans = 2; uid.numTevStages = 16;
        uid.numIndStages = 4; uid.alphaFunc = 5 | 3 << 3; uid.fogSel = 14; uid.zTex = 6;
        for (unsigned i = 0; i < 4; ++i) uid.iref |= (i | i << 3) << (6 * i);
        for (unsigned i = 0; i < 16; ++i) {
            uid.tevOrder[i / 2] |= (i % 8 | i % 8 << 3 | 64) << ((i & 1) * 12);
            uid.tevColor[i] = 2 << 12 | 4 << 8 | 14 << 4 | 6 | i % 3 << 16 | i % 2 << 18 | i % 4 << 20;
            uid.tevAlpha[i] = 1 << 13 | 2 << 10 | 6 << 7 | 3 << 4 | i % 3 << 16 | i % 2 << 18 | i % 4 << 20;
            uid.ksel[i / 2] |= (12 + i % 4) << ((i & 1) * 10 + 4);
            uid.indCmd[i] = (1 + i % 3) << 9 | i % 8 << 4 | i % 4;
        }
        const std::string fragment = gpu::genPortableFragmentShader(uid, gpu::PortableDialect::VitaCg);
        require(fragment.find("#version") == std::string::npos, "Vita output must be Cg, not a GLSL compatibility label");
        require(fragment.find("typedef float4 vec4;") != std::string::npos, "full float Cg types");
        require(fragment.find(" : DEPTH;") != std::string::npos, "Z texture depth is not dropped");
        require(fragment.find("// stage 15") != std::string::npos, "all sixteen TEV stages retained");
        require(fragment.find("tex2Dbias") != std::string::npos, "texture LOD bias retained");
        require(fragment.find("fmod") == std::string::npos && fragment.find("half") == std::string::npos,
                "no truncating modulo or reduced precision");
        gpu::PixelUniformLayout layout; std::string error;
        require(gpu::makePixelUniformLayout(uid, layout, error), "full material uniform layout");
        require(layout.vectors == 25, "no nominal sixteen-vector truncation");
        if(texgens==8) wide=uid;
        require(fragment.find("uniform vec4 u_pixel[25]") != std::string::npos, "all live uniforms retained");
        for (unsigned sampler = 0; sampler < 8; ++sampler)
            require(fragment.find("TEXUNIT" + std::to_string(sampler)) != std::string::npos, "explicit sampler units");
        std::ofstream(output / ("tev" + std::to_string(texgens) + "_f.cg")) << fragment;
        const std::string vertex = gpu::genVitaCpuVertexShader(uid);
        require(vertex.find("result.position=a_clip;") != std::string::npos &&
                vertex.find("result.position.z=") == std::string::npos,
                "clip position retained, without a second viewport depth conversion");
        for (unsigned texgen = 0; texgen < texgens; ++texgen)
            require(vertex.find("float4 v_tex" + std::to_string(texgen) + " : TEXCOORD" + std::to_string(texgen)) != std::string::npos,
                    "all STQ inputs remain full float, with matching fragment semantics");
        std::ofstream(output / ("tev" + std::to_string(texgens) + "_v.cg")) << vertex;
    }
    for (unsigned depth = 0; depth <= 6; ++depth) {
        gpu::ShaderUid uid{};
        uid.numTevStages = 1; uid.alphaFunc = 63; uid.zTex = depth;
        const auto source = gpu::genPortableFragmentShader(uid, gpu::PortableDialect::VitaCg);
        std::ofstream(output / ("depth" + std::to_string(depth) + "_f.cg")) << source;
        if ((depth >> 2) == 1)
            require(source.find("float fragmentZ=gl_FragCoord.z") != std::string::npos,
                    "Z24 add retains current window depth without GLSL precision token");
    }
    gpu::ShaderUid invalid{};
    require(gpu::genPortableFragmentShader(invalid, gpu::PortableDialect::VitaCg).empty(), "invalid UID rejected");
    require(gpu::genVitaCpuVertexShader(invalid).empty(), "invalid vertex UID rejected");
    // Keep legacy fixture files unchanged: these new sources have a distinct
    // program/reflection contract and compiler acceptance is checked separately.
    require(&gpu::genPortableRasterFragmentShader != nullptr, "exact raster fragment generator exists");
    gpu::ShaderUid raster_uid{};
    raster_uid.numTevStages = 1; raster_uid.alphaFunc = 63;
    const auto raster_source = gpu::genPortableRasterFragmentShader(raster_uid, gpu::PortableDialect::VitaCg);
    require(raster_source.find("uniform vec4 u_raster[2]") != std::string::npos, "independent reflected raster vectors");
    require(raster_source.find("gl_FragCoord.x>=u_raster[0].z") != std::string::npos &&
            raster_source.find("gl_FragCoord.y>=u_raster[0].w") != std::string::npos, "exclusive pixel scissor right/bottom");
    require(raster_source.find("outc.a=u_raster[1].y") != std::string::npos, "alpha override follows original TEV/alpha testing");
    std::ofstream(output / "raster0_f.cg") << raster_source;
    const auto describeManual=static_cast<bool(*)(const gpu::ShaderUid&,gpu::VitaFragmentProfile,
        gpu::VitaFragmentLayout&,std::string&,uint32_t)>(&gpu::describeVitaFragment);
    const auto generateManual=static_cast<std::string(*)(const gpu::ShaderUid&,gpu::VitaFragmentProfile,uint32_t)>(&gpu::genVitaFragmentShader);
    require(describeManual && generateManual,"typed manual-sampling shader/layout APIs exist separately from implicit material sources");
    gpu::ShaderUid sampled{}; sampled.numTexGens=2; sampled.numTevStages=1; sampled.alphaFunc=63;
    sampled.tevOrder[0]=64;
    sampled.tevColor[0]=(15u<<12)|(15u<<8)|(15u<<4)|8u|(1u<<19);
    sampled.tevAlpha[0]=(7u<<13)|(7u<<10)|(7u<<7)|(4u<<4)|(1u<<19);
    gpu::VitaFragmentLayout base,manual; std::string reason;
    require(gpu::describeVitaFragment(sampled,gpu::VitaFragmentProfile::Rgba6AddSnapshot,base,reason) &&
        describeManual(sampled,gpu::VitaFragmentProfile::Rgba6AddSnapshot,manual,reason,1) &&
        manual.pixel.vectors==base.pixel.vectors+1 && manual.pixel.textureMask==base.pixel.textureMask &&
        manual.pixel.slots==base.pixel.slots && manual.snapshotSampler==base.snapshotSampler && manual.snapshotSlot==base.snapshotSlot,
        "manual parameters append without displacing original uniforms, texture units or the ordered snapshot");
    const auto source=generateManual(sampled,gpu::VitaFragmentProfile::Rgba6AddSnapshot,1);
    require(!source.empty() && source!=gpu::genVitaFragmentShader(sampled,gpu::VitaFragmentProfile::Rgba6AddSnapshot) &&
        source.find("gxManualSample(s_tex0,")!=std::string::npos && source.find("tex2Dlod")!=std::string::npos &&
        source.find("ddx(")!=std::string::npos && source.find("ddy(")!=std::string::npos &&
        source.find("sign(")!=std::string::npos && source.find("16384.0")!=std::string::npos &&
        source.find("tevin_d=vec4(tex.rgb,tex.a);")!=std::string::npos,
        "real Cg lowering uses original-coordinate diagonal derivatives, signed quantization and byte-first point-fetch filtering");
    require(!describeManual(sampled,gpu::VitaFragmentProfile::Raster,manual,reason,2) &&
        !describeManual(sampled,gpu::VitaFragmentProfile::Raster,manual,reason,256) &&
        generateManual(sampled,gpu::VitaFragmentProfile::Raster,2).empty(),
        "dead or out-of-range manual units cannot substitute a different material");
    require(describeManual(wide,gpu::VitaFragmentProfile::Raster,manual,reason,3) && manual.pixel.vectors==27 &&
        !describeManual(wide,gpu::VitaFragmentProfile::Raster,manual,reason,7),
        "all original wide-material uniforms fit exactly or fail without dropping an appended sampler contract");
    sampled.numIndStages=1; sampled.iref=1|(1<<3); sampled.indCmd[0]=0x270;
    require(describeManual(sampled,gpu::VitaFragmentProfile::Rgba6AddSnapshot,manual,reason,1),
        "original eight-bit indirect matrix/bias tuple is retained with post-offset manual LOD");
    for(const auto bit:{1u<<2,1u<<7,1u<<11,1u<<13,1u<<16,1u<<19,1u<<20}) {
        sampled.indCmd[0]=0x270|bit;
        require(!describeManual(sampled,gpu::VitaFragmentProfile::Raster,manual,reason,1),
            "manual sampling rejects unimplemented indirect format/bump/matrix-id/wrap/UTC/previous-offset semantics");
    }
    sampled.indCmd[0]=0x270;
    std::ofstream(output / "manual_additive_f.cg") << generateManual(sampled,gpu::VitaFragmentProfile::Rgba6AddSnapshot,1);
    std::ofstream(output / "manual_additive_v.cg") << gpu::genVitaCpuVertexShader(sampled);
    puts("Vita Cg contracts: 9 full 16-stage, 8-sampler, 25-uniform materials; not GPU rendering evidence");
}
