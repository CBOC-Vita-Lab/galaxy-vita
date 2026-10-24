#pragma once
#include "draw_spans.h"
#include "gx_ram_texture.h"
#include "../../../vita/runtime/gpu_texture.h"

namespace gpu {
struct VitaTextureCache;
// Explicit fallible ownership: no GPU deletion from a shared_ptr destructor.
int createVitaTextureCache(PortVitaGxmDevice*, VitaTextureCache**);
// Image bindings only. EFB address/copy history remains the copier's contract.
// One upload per immutable image/version; distinct samplers share those pixels.
// Returned views are borrowed; only this cache can release their native owners.
int resolveVitaImage(VitaTextureCache*, const Frame&, const SpanTexture&,
                     PortVitaGxmTextureView*&, std::shared_ptr<const void>& lifetime,
                     std::string& error,bool explicitLinearMip=false);
// Exact copy origin plus complete authoritative draw-time RAM/TLUT content.
// Native views retain immutable decoded provenance for replay validation.
int resolveVitaRamImage(VitaTextureCache*,uint64_t frame,const SpanTexture&,const SpanRegisters&,unsigned unit,
                        const VitaRamTextures&,PortVitaGxmTextureView*&,std::shared_ptr<const void>& lifetime,
                        std::shared_ptr<const GxRamTexture>& provenance,std::string& error,bool explicitLinearMip=false);
// Retires only unleased entries, scene-closed/Finish-gated. Failure retains owner.
int collectVitaTextures(VitaTextureCache*);
// A failed close keeps the cache pointer; resolve rejects a closing cache.
int releaseVitaTextureCache(VitaTextureCache**);
}
