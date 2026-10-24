// Native frame traversal uses the game enum ABI and only scalar SDK adapters.
#pragma once
#include "cpu_xf.h"
#include "index_chunks.h"
#include "portable_shader.h"
#include "vita_material_cache.h"
#include "vita_texture_cache.h"
#include "vita_efb.h"
#include "../../../vita/runtime/gpu_raster.h"
#include "../../../vita/runtime/gpu_staging.h"
#include "../../../vita/runtime/gpu_texture.h"
#include <cstdio>

namespace gpu {

struct VitaFrameError {
    size_t sourceEventIndex = 0, drawIndex = 0, commandWord = 0;
    uint32_t vertexOrdinal = 0;
    int nativeCode = 0;
    std::string message;
};

struct VitaPreparedDraw {
    std::vector<PortVitaGxmVertex> vertices;
    PixelUniformLayout layout;
    VitaFragmentProfile profile=VitaFragmentProfile::Raster;
    VitaFragmentLayout fragment;
    std::array<float, 108> pixel{};
    PortVitaGxmRaster raster{};
    PortVitaGxmBlend blend{};
    uint32_t topology = 0;
};

// Gather the complete sparse remap, full CPU XF and exact compact TEV state.
// Failure clears output, with original frame/event/draw/vertex provenance.
bool prepareVitaDraw(const IndexChunks& source, size_t chunkIndex,
                     VitaPreparedDraw& output, VitaFrameError& error);

struct VitaShaderRequest {
    ShaderUid uid{};
    VitaFragmentProfile profile=VitaFragmentProfile::Raster;
    uint32_t manualTextureMask=0, pixelVectors=0, occurrences=0;
    size_t firstCommandWord=0;
    bool vertexMissing=false, fragmentMissing=false;
};
struct VitaShaderInventory {
    std::vector<VitaShaderRequest> requests;
    std::vector<VitaFrameError> rejections;
    size_t inspectedChunks=0, rejectedChunks=0;
    bool complete=false, limitReached=false;
};
// Diagnostic shader metadata only: no XF, pixel reads, uploads or native calls.
// Bounds publish an explicitly incomplete inventory, never a truncated success.
bool inventoryVitaFrameShaders(const IndexChunks&,const VitaShaderLibrary&,VitaShaderInventory&);
// Private JSONL records use a validated four-digit tag and a checked flush.
bool writeVitaShaderInventory(FILE*,uint64_t frame,const char* tag,const VitaShaderInventory&);

struct VitaShaderCapture {
    char tag[5]{};
    bool attempted=false;
};
// Missing or malformed markers disable capture; each configured tag attempts once.
bool configureVitaShaderCapture(FILE*,VitaShaderCapture&);
// Exclusive private output. Zero means disabled; negative means not saved.
int captureVitaShaderInventory(const char* directory,uint64_t frame,const VitaShaderInventory&,VitaShaderCapture&);

struct VitaDrawBinding {
    VitaMaterial* material = nullptr;
    PortVitaGxmTextureView* views[8]{};
    std::shared_ptr<const GxRamTexture> ramSources[8];
    // Keeps the program and all descriptors/images alive until a successful
    // scene submission AND GPU Finish. Cache owners must honor this lease.
    std::shared_ptr<const void> lifetime;
};

// Resolves the exact material and every UID-used immutable image/sampler.
// Scene-closed; publishes nothing on failure. Native rollback stays cache-owned.
int resolveVitaDraw(PortVitaGxmDevice*, VitaMaterialCache*, VitaTextureCache*, const VitaShaderLibrary&,
                    const IndexChunks&, const IndexChunk&, const VitaPreparedDraw&,
                    VitaDrawBinding&, std::string& error);
int resolveVitaDrawWithRam(PortVitaGxmDevice*,VitaMaterialCache*,VitaTextureCache*,const VitaShaderLibrary&,
                          const IndexChunks&,const IndexChunk&,const VitaPreparedDraw&,const VitaRamTextures&,
                          VitaDrawBinding&,std::string& error);

// The material/texture cache and EFB implementation provide these operations.
// Missing capabilities return an error, never a placeholder or skipped copy.
// Resolve and surface run scene-closed; marker/camera are metadata only.
class VitaFrameBackend {
public:
    virtual ~VitaFrameBackend() = default;
    virtual int resolve(const IndexChunks&, const IndexChunk&, const VitaPreparedDraw&,
                        VitaDrawBinding&, std::string& error) = 0;
    virtual int surface(uint32_t width, uint32_t height, PortVitaGxmSurface*&,
                        std::shared_ptr<const void>& lifetime, std::string& error) = 0;
    virtual int snapshot(PortVitaGxmSurface*,uint32_t,uint32_t,
                         std::shared_ptr<const VitaEfbBlendSnapshot>&,std::string& error) {
        error="frame backend has no owned ordered blend snapshot strategy"; return -1;
    }
    // Runs after EndScene and Finish, before resolving any post-copy texture.
    virtual int copy(const Frame&, const SpanCopy&, std::string& error) = 0;
    virtual int marker(const Frame&, const SpanMarker&, std::string& error) = 0;
    virtual int camera(const Frame&, const SpanCamera&, std::string& error) = 0;
};

struct VitaFrameReplay;
// One submission thread. Device outlives replay; replay owns its mapped staging.
// Only this replay may submit/end its scenes. No raw device/program bypass.
int createVitaFrameReplay(PortVitaGxmDevice*, VitaFrameReplay** output);
// Copy-delimited batches: pre-upload outside a scene, draw in original order,
// close/fence, execute exact copy, then resolve the next batch. No automatic
// replay of a partially submitted frame. Failed EndScene retains all leases.
bool submitVitaFrame(VitaFrameReplay*, std::shared_ptr<const IndexChunks>,
                     VitaFrameBackend&, VitaFrameError& error);
// Ends/fences retained work without replaying callbacks or drawing it again.
// Failure keeps the complete frame/resource ownership for a later retry.
int discardVitaFrame(VitaFrameReplay*);
int releaseVitaFrameReplay(VitaFrameReplay**);

} // namespace gpu
