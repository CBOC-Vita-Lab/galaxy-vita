#pragma once
#include "gx_copy_bytes.h"

namespace gpu {
// Caller-owned authoritative draw-time RAM/TLUT windows. Stable and exclusively
// ordered for the whole resolution call, not live global RAM from async replay.
// copySequence is the last committed copy included by this snapshot.
struct GxTextureMemory {
    const unsigned char* ram=nullptr;
    uint32_t ramBase=0;
    size_t ramBytes=0;
    const unsigned char* tlut=nullptr;
    uint32_t tlutBase=0;
    size_t tlutBytes=0;
    uint64_t frame=0,copySequence=0;
};
struct GxRamTextureLayout {
    uint32_t address=0,format=0,width=0,height=0,levels=0;
    uint32_t paletteAddress=0,paletteFormat=0,paletteBytes=0;
    uint32_t packedBytes=0,texels=0;
    bool operator==(const GxRamTextureLayout&) const = default;
};
int describeGxRamTexture(const SpanRegisters&,unsigned unit,GxRamTextureLayout&,std::string& error);
class GxRamTexture {
public:
    GxRamTexture()=default;
    const GxRamTextureLayout& layout() const { return mLayout; }
    const uint32_t* rgba() const { return mRgba.get(); }
    uint64_t frame() const { return mFrame; }
    uint64_t copySequence() const { return mCopySequence; }
    const std::shared_ptr<const GxCopyBytes>& origin() const { return mOrigin; }
    // Exact complete packed bytes and palette, never a prefix/hash/revision hint.
    bool matches(const GxRamTextureLayout&,const GxTextureMemory&,
                 const std::shared_ptr<const GxCopyBytes>& origin) const;
private:
    friend int decodeGxRamTexture(const SpanRegisters&,unsigned,const GxTextureMemory&,
                                 std::shared_ptr<const GxCopyBytes>,GxRamTexture&,std::string&);
    GxRamTextureLayout mLayout;
    std::unique_ptr<unsigned char[]> mPacked;
    std::unique_ptr<uint16_t[]> mPalette;
    std::unique_ptr<uint32_t[]> mRgba;
    std::shared_ptr<const GxCopyBytes> mOrigin;
    uint64_t mFrame=0,mCopySequence=0;
};
// Owns immutable packed/palette/decoded bytes; uses actual RAM, not the original
// packet's contiguous rows. Includes overlaps, old tails, gaps and CPU changes.
// Missing memory/palette, manual TMEM and reserved formats reject, no fallback.
int decodeGxRamTexture(const SpanRegisters&,unsigned unit,const GxTextureMemory&,
                       std::shared_ptr<const GxCopyBytes> origin,GxRamTexture&,std::string& error);
struct VitaRamTextures {
    GxCopyHistory* history=nullptr;
    GxTextureMemory memory;
    // Optional original-address region query for disjoint native arenas. Only
    // RAM fields may change; TLUT/frame/copy cut remain this ordered snapshot.
    bool (*window)(uint32_t,GxTextureMemory&,std::string&)=nullptr;
};
}
