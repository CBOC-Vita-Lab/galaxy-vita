#include "gx_ram_texture.h"
#include "tex_decode.h"
#include "../../../vita/runtime/gpu_texture.h"
#include <algorithm>
#include <cstring>
#include <new>

namespace gpu {
namespace {
constexpr int invalid=-1;
const unsigned char* window(const unsigned char* data,uint32_t base,size_t bytes,
                            uint32_t address,uint32_t length) {
    if (!data || bytes>UINT64_C(0x100000000)-base || address<base) return nullptr;
    const size_t offset=size_t(address-base);
    if (offset>bytes || length>bytes-offset) return nullptr;
    return data+offset;
}
bool originValid(const GxRamTextureLayout& layout,const GxTextureMemory& memory,
                 const std::shared_ptr<const GxCopyBytes>& origin) {
    return origin && origin->data() && origin->sequence() && origin->address()==layout.address &&
        origin->frame()<=memory.frame && origin->sequence()<=memory.copySequence;
}
}

int describeGxRamTexture(const SpanRegisters& registers,unsigned unit,GxRamTextureLayout& out,std::string& error) {
    error.clear();
    auto fail=[&](const char* message) { error=message; return invalid; };
    if (unit>=8) return fail("RAM texture unit is outside GX hardware slots");
    const unsigned base=(unit<4 ? 0x80 : 0xa0)+(unit&3);
    const uint32_t image0=registers.bp[base+8],image1=registers.bp[base+12],image3=registers.bp[base+20];
    if ((image0|image1|image3)&0xff000000) return fail("RAM texture registers exceed GX BP width");
    if (image1&(1u<<21)) return fail("manual TMEM textures need an explicit TMEM snapshot, not RAM substitution");
    PortVitaGxmSampler sampler{};
    if (port_vita_gxm_sampler_decode(registers.bp[base],registers.bp[base+4],&sampler)<0)
        return fail("RAM texture has a reserved GX sampler state");
    GxRamTextureLayout layout;
    layout.address=image3<<5; layout.width=(image0&1023)+1; layout.height=((image0>>10)&1023)+1;
    layout.format=(image0>>20)&15;
    switch (layout.format) {
    case TF_I4: case TF_I8: case TF_IA4: case TF_IA8: case TF_RGB565: case TF_RGB5A3:
    case TF_RGBA8: case TF_CI4: case TF_CI8: case TF_CI14X2: case TF_CMPR: break;
    default: return fail("RAM texture format is reserved");
    }
    layout.levels=1;
    uint32_t width=layout.width,height=layout.height;
    const uint32_t requested=sampler.mip_filter ? (sampler.max_units+15)/16+1 : 1;
    while (layout.levels<requested && (width>1 || height>1)) {
        width=std::max(1u,width/2); height=std::max(1u,height/2); ++layout.levels;
    }
    width=layout.width; height=layout.height;
    for (unsigned level=0;level<layout.levels;++level) {
        layout.packedBytes+=uint32_t(texLevelSize(layout.format,width,height));
        layout.texels+=width*height;
        width=std::max(1u,width/2); height=std::max(1u,height/2);
    }
    if (layout.format==TF_CI4 || layout.format==TF_CI8 || layout.format==TF_CI14X2) {
        const uint32_t tlut=registers.bp[base+24];
        if (tlut&~0xfffu) return fail("RAM texture palette register contains reserved bits");
        layout.paletteAddress=(tlut&1023)<<9; layout.paletteFormat=(tlut>>10)&3;
        if (layout.paletteFormat==3) return fail("RAM texture palette format is reserved");
        layout.paletteBytes=layout.format==TF_CI4 ? 32 : layout.format==TF_CI8 ? 512 : 32768;
    }
    out=layout; return 0;
}

bool GxRamTexture::matches(const GxRamTextureLayout& layout,const GxTextureMemory& memory,
                           const std::shared_ptr<const GxCopyBytes>& origin) const {
    if (!mRgba || !mPacked || mLayout!=layout || mFrame!=memory.frame || mCopySequence!=memory.copySequence ||
        mOrigin!=origin || !originValid(layout,memory,origin)) return false;
    const auto* packed=window(memory.ram,memory.ramBase,memory.ramBytes,layout.address,layout.packedBytes);
    if (!packed || std::memcmp(mPacked.get(),packed,layout.packedBytes)) return false;
    if (!layout.paletteBytes) return true;
    const auto* palette=window(memory.tlut,memory.tlutBase,memory.tlutBytes,layout.paletteAddress,layout.paletteBytes);
    return palette && mPalette && !std::memcmp(mPalette.get(),palette,layout.paletteBytes);
}

int decodeGxRamTexture(const SpanRegisters& registers,unsigned unit,const GxTextureMemory& memory,
                       std::shared_ptr<const GxCopyBytes> origin,GxRamTexture& out,std::string& error) {
    error.clear();
    auto fail=[&](const char* message) { error=message; return invalid; };
    if (out.rgba()) return fail("RAM texture decoder cannot replace a published snapshot");
    GxRamTexture candidate;
    if (describeGxRamTexture(registers,unit,candidate.mLayout,error)<0) return invalid;
    const auto& layout=candidate.mLayout;
    if (!originValid(layout,memory,origin)) return fail("RAM texture copy origin is missing, future or beyond the snapshot cut");
    const auto* packed=window(memory.ram,memory.ramBase,memory.ramBytes,layout.address,layout.packedBytes);
    if (!packed) return fail("RAM texture needs its complete original padded mip extent");
    const auto* palette=layout.paletteBytes ?
        window(memory.tlut,memory.tlutBase,memory.tlutBytes,layout.paletteAddress,layout.paletteBytes) : nullptr;
    if (layout.paletteBytes && !palette) return fail("RAM texture needs the complete authoritative TLUT bank");
    candidate.mPacked.reset(new(std::nothrow) unsigned char[layout.packedBytes]);
    candidate.mRgba.reset(new(std::nothrow) uint32_t[layout.texels]);
    if (layout.paletteBytes) candidate.mPalette.reset(new(std::nothrow) uint16_t[layout.paletteBytes/2]);
    if (!candidate.mPacked || !candidate.mRgba || (layout.paletteBytes && !candidate.mPalette))
        return fail("RAM texture snapshot allocation failed");
    std::memcpy(candidate.mPacked.get(),packed,layout.packedBytes);
    if (palette) std::memcpy(candidate.mPalette.get(),palette,layout.paletteBytes);
    uint32_t width=layout.width,height=layout.height,offset=0,texels=0;
    for (unsigned level=0;level<layout.levels;++level) {
        decodeTexture(reinterpret_cast<uint8_t*>(candidate.mRgba.get()+texels),candidate.mPacked.get()+offset,
                      layout.format,width,height,candidate.mPalette.get(),layout.paletteFormat);
        offset+=uint32_t(texLevelSize(layout.format,width,height)); texels+=width*height;
        width=std::max(1u,width/2); height=std::max(1u,height/2);
    }
    candidate.mFrame=memory.frame; candidate.mCopySequence=memory.copySequence; candidate.mOrigin=std::move(origin);
    out=std::move(candidate); return 0;
}
}
