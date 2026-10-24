// API-independent pixel uniforms and full-precision ES100 TEV lowering.
#pragma once

#include <array>
#include <string>

#include "gl_shadergen.h"

namespace gpu {

enum class PortableDialect { Es100, Es320Diagnostic, VitaCg };
#define GPU_VITA_FRAGMENT_PROFILES 1
enum class VitaFragmentProfile : uint32_t { Raster=0, Rgba6SrcAlphaSnapshot=1, Rgba6AddSnapshot=2, Rgba6SrcAlphaDitherSnapshot=3, Rgba6SrcAlphaAddSnapshot=4 };
constexpr bool isKnownVitaFragmentProfile(VitaFragmentProfile profile) {
    switch(profile) {
    case VitaFragmentProfile::Raster:
    case VitaFragmentProfile::Rgba6SrcAlphaSnapshot:
    case VitaFragmentProfile::Rgba6AddSnapshot:
    case VitaFragmentProfile::Rgba6SrcAlphaDitherSnapshot:
    case VitaFragmentProfile::Rgba6SrcAlphaAddSnapshot: return true;
    default: return false;
    }
}

struct PixelUniformLayout {
    // PixelState vec4 source -> compact uniform slot, or -1 if UID-dead.
    std::array<int, 27> slots{};
    uint32_t vectors = 0, textureMask = 0;
};

struct VitaFragmentLayout {
    PixelUniformLayout pixel;
    uint32_t snapshotSampler=8,snapshotSlot=27;
    uint32_t manualTextureMask=0;
    std::array<int,8> manualSlots{-1,-1,-1,-1,-1,-1,-1,-1};
};

// Snapshot profiles append resources without changing any original TEV slot.
bool describeVitaFragment(const ShaderUid&,VitaFragmentProfile,VitaFragmentLayout&,std::string& error,uint32_t manualTextureMask=0);
std::string genVitaFragmentShader(const ShaderUid&,VitaFragmentProfile,uint32_t manualTextureMask=0);

// Reject malformed/VR UIDs rather than emitting a partial-fidelity program.
bool makePixelUniformLayout(const ShaderUid& uid, PixelUniformLayout& layout, std::string& error);
bool packPixelUniforms(const PixelUniformLayout& layout, const PixelState& state,
                       std::array<float, 108>& output);
// Caller still verifies matching destination/texture extents and GPU ownership.
bool packVitaFragmentUniforms(const ShaderUid&,VitaFragmentProfile,const PixelState&,uint32_t width,uint32_t height,
    bool alphaOverride,uint32_t alphaByte,std::array<float,108>& output,std::string& error,uint32_t manualTextureMask=0);

// ES100 uses ordinary uniforms/attributes, no SSBO, integer ALU or record index.
// Actual Vita compiler reflection must still approve the uniform/varying budget.
std::string genPortableFragmentShader(const ShaderUid& uid, PortableDialect dialect);
// Same TEV math with exact scissor, final alpha override and unblended/no-fog
// RGBA6/Bayer storage in u_raster[2]. Native replay gates blend/fog strategies.
// Legacy sources stay stable.
std::string genPortableRasterFragmentShader(const ShaderUid& uid, PortableDialect dialect);
std::string genPortableCpuVertexShader(const ShaderUid& uid);
// Direct Cg with explicit matching varyings/samplers; clip depth is unchanged.
// Attribute names are reflected into ClipVertex, never positional guesses.
std::string genVitaCpuVertexShader(const ShaderUid& uid);

} // namespace gpu
