#pragma once
#include "portable_shader.h"
#include "../../../vita/runtime/gpu_device.h"
#include <unordered_map>
#include <vector>
#include <memory>

namespace gpu {
enum class VitaShaderStage : uint32_t { Vertex, Fragment };
struct VitaShaderArtifact {
    VitaShaderStage stage=VitaShaderStage::Vertex;
    std::string source;
    std::vector<unsigned char> gxp;
    uint32_t precisionFlags=0; // any fast-math/precision/integer flag rejects
};

// CPU-only library of trusted compiler-produced blobs. Offline tooling verifies
// source/blob hashes and compiler flags before generating its installed entries.
// No raw serialized/untrusted GXP loader; the native SDK checks actual reflection.
class VitaShaderLibrary {
private:
    std::unordered_map<std::string,std::vector<unsigned char>> vertex,fragment;
    friend bool installVitaShader(VitaShaderLibrary&,VitaShaderArtifact,std::string&);
    friend const std::vector<unsigned char>* findVitaShader(const VitaShaderLibrary&,VitaShaderStage,const std::string&);
};
bool installVitaShader(VitaShaderLibrary&, VitaShaderArtifact, std::string& error);
const std::vector<unsigned char>* findVitaShader(const VitaShaderLibrary&, VitaShaderStage, const std::string& source);
// Baked, build-local verified bundle. Missing/stale source pairs reject visibly.
bool installBakedVitaShaders(VitaShaderLibrary&, std::string& error);

struct VitaMaterialCache;
struct VitaMaterial;
int createVitaMaterialCache(PortVitaGxmDevice*, VitaMaterialCache**);
int resolveVitaMaterial(VitaMaterialCache*, const ShaderUid&, const PortVitaGxmBlend&,
                        const VitaShaderLibrary&, VitaMaterial*&, std::shared_ptr<const void>& lifetime,
                        std::string& error);
// Borrowed typed material; exact UID/blend/layout/device before native use.
int bindVitaMaterial(const VitaMaterial*, PortVitaGxmDevice*, const ShaderUid&,
                     const PortVitaGxmBlend&, const PixelUniformLayout&,
                     PortVitaGxmProgram*&, std::string& error);
// Logical blend identity stays immutable even when a profile disables the
// fixed-function blend. The legacy APIs are Raster-only wrappers.
int resolveVitaMaterialProfile(VitaMaterialCache*,const ShaderUid&,const PortVitaGxmBlend&,VitaFragmentProfile,
    const VitaShaderLibrary&,VitaMaterial*&,std::shared_ptr<const void>& lifetime,std::string& error,uint32_t manualTextureMask=0);
int bindVitaMaterialProfile(const VitaMaterial*,PortVitaGxmDevice*,const ShaderUid&,const PortVitaGxmBlend&,
    VitaFragmentProfile,const VitaFragmentLayout&,PortVitaGxmProgram*&,std::string& error);
// No fallible GPU deletion in shared_ptr destructors. Leases pin cache entries.
int collectVitaMaterials(VitaMaterialCache*);
int releaseVitaMaterialCache(VitaMaterialCache**);
}
