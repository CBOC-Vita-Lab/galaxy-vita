#pragma once
#include "draw_spans.h"
#include <cstddef>

namespace gpu {
struct GxCopyHistory;
struct GxCopyGeometry {
    uint32_t left=0,top=0,sourceWidth=0,sourceHeight=0;
    uint32_t width=0,height=0,paddedWidth=0,paddedHeight=0,format=0;
    uint32_t blockWidth=0,blockHeight=0,blockBytes=0;
};
// Shared original command/tile geometry for the pixel producer and packer.
// Success-only publication; does not validate a particular RAM/input window.
int describeGxTextureCopy(const SpanCopy&,GxCopyGeometry&,std::string& error);
// Linear YUYV422 geometry. Currently accepts the game's non-AA progressive
// unity-scale path; unproven field/scaled/odd-chroma strategies fail explicitly.
int describeGxXfbCopy(const SpanCopy&,GxCopyGeometry&,std::string& error);

// Final copy-stage RGBA bytes, top-left row-major, after original EFB precision,
// half sampling, vertical filtering, gamma and intensity/YUV as applicable.
// Texture width/height MUST include the complete destination tile padding; padding
// samples are original copy samples, not zeros or copies of the last texel.
// XFB input contains exactly the filtered/gamma-corrected RGB rectangle, before
// YUYV422 conversion. A raw native capture is NOT automatically processed input.
struct GxCopyTexels {
    const unsigned char* data=nullptr;
    uint32_t width=0,height=0,stride=0;
    size_t bytes=0;
};
// Explicit CPU-accessible RAM window in original physical address space.
// The native caller must supply its verified arena/address translation and
// scheduler ordering. No global RAM pointer or native cooked address is guessed.
struct GxCopyMemory {
    unsigned char* data=nullptr;
    uint32_t baseAddress=0;
    size_t bytes=0;
};

class GxCopyBytes {
public:
    GxCopyBytes()=default;
    const SpanCopy& command() const { return mCommand; }
    uint64_t frame() const { return mFrame; }
    uint64_t sequence() const { return mSequence; }
    uint32_t address() const { return mAddress; }
    uint32_t width() const { return mWidth; }
    uint32_t height() const { return mHeight; }
    uint32_t format() const { return mFormat; }
    bool isXfb() const { return (mCommand.words[9]&(1u<<14))!=0; }
    uint32_t rowBytes() const { return mRowBytes; }
    uint32_t rows() const { return mRows; }
    uint32_t ramStride() const { return mRamStride; }
    uint32_t ramExtent() const { return mRamExtent; }
    const unsigned char* data() const { return mData.get(); }
    size_t bytes() const { return size_t(mRowBytes)*mRows; }
private:
    friend struct GxCopyHistory;
    SpanCopy mCommand{};
    uint64_t mFrame=0,mSequence=0;
    uint32_t mAddress=0,mWidth=0,mHeight=0,mFormat=0;
    uint32_t mRowBytes=0,mRows=0,mRamStride=0,mRamExtent=0;
    std::unique_ptr<unsigned char[]> mData;
    std::shared_ptr<GxCopyBytes> mNext;
};

int createGxCopyHistory(GxCopyHistory**);
// Validate and allocate before touching RAM. Packs original texture-copy
// layouts or progressive linear XFB, writes rows in increasing order (including overlapping/zero
// strides), then publishes immutable bytes/history/lease without fallible work.
// Rows in data() are contiguous; RAM gaps are NOT packet bytes. This does not
// implement VI display, pixel production, memory-shadow reinterpretation or GPU clear.
// A frame/copy-ID duplicate or decreasing frame rejects without replay/writes.
int storeGxCopyBytes(GxCopyHistory*,uint64_t frame,const SpanCopy&,const GxCopyTexels&,
                      const GxCopyMemory&,std::shared_ptr<const GxCopyBytes>&,std::string& error);
// Exact copy ID/address, newest origin not later than the requesting frame.
// No dimensions-only/address-only fallback to an unrelated or future version.
int lookupGxCopyBytes(GxCopyHistory*,uint64_t requestingFrame,uint32_t copyId,uint32_t address,
                       std::shared_ptr<const GxCopyBytes>&,std::string& error);
// Retain leases, current-frame replay guards and the latest origin for each
// base address and copy kind (texture/XFB), even across frames. The recorder's
// texture origins survive XFB overwrites; later texture reads decode current RAM.
// Single ordered CPU owner; no concurrent
// store/lookup/collection or mutation of producer input is permitted.
// Retired cutoff must come from actual pending-frame/lifetime ownership.
// Partial overlaps and CPU mutations still need the eventual RAM-shadow path.
int collectGxCopyBytes(GxCopyHistory*,uint64_t retiredThroughFrame);
// CPU-only retirement, not a GPU fence. Outstanding leases retain close owner;
// close stops publication/lookup and can be retried after leases are dropped.
int releaseGxCopyHistory(GxCopyHistory**);
}
