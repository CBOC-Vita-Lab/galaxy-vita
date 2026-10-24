#include "gx_copy_bytes.h"
#include <new>
#include <cstring>
#include <cstdlib>
#include <cstdint>

namespace gpu {
namespace {
int describeRectangle(const SpanCopy& copy,GxCopyGeometry& geometry,std::string& error) {
    error.clear();
    auto fail=[&](const char* text) { error=text; return -1; };
    for (unsigned i=1;i<10;++i) if (copy.words[i]>0xFFFFFF)
        return fail("copy BP payload exceeds 24 bits");
    if (copy.filter[0]>0xFFFFFF || copy.filter[1]>0xFFFFFF || copy.colorMode>0xFFFFFF ||
        copy.depthMode>0xFFFFFF || copy.pixelFormat>0x7F || (copy.pixelFormat&7)>3 ||
        ((copy.pixelFormat>>3)&7)>3)
        return fail("invalid command-time copy pixel state");
    geometry.left=copy.words[1]&1023; geometry.top=(copy.words[1]>>10)&1023;
    geometry.sourceWidth=(copy.words[2]&1023)+1; geometry.sourceHeight=((copy.words[2]>>10)&1023)+1;
    if (!copy.efbWidth || !copy.efbHeight || copy.efbWidth>1024 || copy.efbHeight>1024 ||
        geometry.left>copy.efbWidth || geometry.top>copy.efbHeight ||
        geometry.sourceWidth>copy.efbWidth-geometry.left || geometry.sourceHeight>copy.efbHeight-geometry.top)
        return fail("copy rectangle is outside command-time EFB");
    return 0;
}
}
int describeGxTextureCopy(const SpanCopy& copy,GxCopyGeometry& out,std::string& error) {
    GxCopyGeometry geometry;
    const int result=describeRectangle(copy,geometry,error); if (result<0) return result;
    const uint32_t control=copy.words[9];
    if (control&(1u<<14)) { error="linear XFB conversion is not tiled texture-copy encoding"; return -1; }
    geometry.format=((control>>4)&7)|(((control>>3)&1)<<3);
    if (geometry.format>12) { error="reserved texture-copy format"; return -1; }
    const uint32_t scale=(control&(1u<<9)) ? 2 : 1;
    geometry.width=(geometry.sourceWidth+scale-1)/scale; geometry.height=(geometry.sourceHeight+scale-1)/scale;
    geometry.blockWidth=geometry.format==0 || geometry.format==1 || geometry.format==2 ||
        (geometry.format>=7 && geometry.format<=10) ? 8 : 4;
    geometry.blockHeight=geometry.format==0 ? 8 : 4;
    geometry.blockBytes=geometry.format==6 ? 64 : 32;
    geometry.paddedWidth=((geometry.width+geometry.blockWidth-1)/geometry.blockWidth)*geometry.blockWidth;
    geometry.paddedHeight=((geometry.height+geometry.blockHeight-1)/geometry.blockHeight)*geometry.blockHeight;
    out=geometry; return 0;
}
int describeGxXfbCopy(const SpanCopy& copy,GxCopyGeometry& out,std::string& error) {
    GxCopyGeometry geometry;
    const int result=describeRectangle(copy,geometry,error); if (result<0) return result;
    const uint32_t control=copy.words[9];
    if (!(control&(1u<<14)) || (control&0x3200)) {
        error="linear XFB requires the original progressive full-width copy strategy"; return -1;
    }
    if ((control&(1u<<10)) && (copy.words[5]&511)!=256) {
        error="non-unity XFB vertical scale requires its original resampling strategy"; return -1;
    }
    if ((geometry.left|geometry.sourceWidth)&1) {
        error="odd original XFB chroma pairing requires a verified hardware strategy"; return -1;
    }
    geometry.width=geometry.paddedWidth=geometry.sourceWidth;
    geometry.height=geometry.paddedHeight=geometry.sourceHeight;
    geometry.format=15; // History tag for linear XFB, never a GX texture format.
    geometry.blockWidth=2; geometry.blockHeight=1; geometry.blockBytes=4;
    out=geometry; return 0;
}
namespace {
constexpr int invalid=-1,noMemory=-2;
struct CopyStorage { alignas(std::max_align_t) unsigned char bytes[256]; };
template<class T> struct CopyAllocator {
    using value_type=T;
    CopyStorage* storage;
    explicit CopyAllocator(CopyStorage* p) noexcept : storage(p) {}
    template<class U> CopyAllocator(const CopyAllocator<U>& other) noexcept : storage(other.storage) {}
    T* allocate(size_t count) noexcept {
        static_assert(sizeof(T)<=sizeof(CopyStorage) && alignof(T)<=alignof(CopyStorage));
        if (count!=1) std::abort();
        return reinterpret_cast<T*>(storage->bytes);
    }
    void deallocate(T*,size_t) noexcept { delete storage; }
    template<class U> bool operator==(const CopyAllocator<U>& other) const noexcept { return storage==other.storage; }
};
struct CopyLayout : GxCopyGeometry {
    uint32_t columns,rows,rowBytes,address,ramStride,ramExtent;
    size_t ramOffset;
};
int copyLayout(const SpanCopy& copy,const GxCopyTexels& input,const GxCopyMemory& memory,
                 CopyLayout& layout,std::string& error) {
    auto fail=[&](const char* text) { error=text; return invalid; };
    const int described=(copy.words[9]&(1u<<14)) ? describeGxXfbCopy(copy,layout,error) :
        describeGxTextureCopy(copy,layout,error);
    if (described<0) return described;
    layout.columns=layout.paddedWidth/layout.blockWidth;
    layout.rows=layout.paddedHeight/layout.blockHeight;
    layout.rowBytes=layout.columns*layout.blockBytes;
    const uint32_t paddedWidth=layout.paddedWidth,paddedHeight=layout.paddedHeight;
    const uint64_t sourceBytes=uint64_t(paddedHeight-1)*input.stride+uint64_t(paddedWidth)*4;
    if (!input.data || input.width!=paddedWidth || input.height!=paddedHeight ||
        input.stride<paddedWidth*4 || sourceBytes>input.bytes)
        return fail("copy input must contain every processed padded RGBA row");
    layout.address=copy.words[3]<<5;
    layout.ramStride=(copy.words[4]&1023)<<5;
    const uint64_t extent=uint64_t(layout.rows-1)*layout.ramStride+layout.rowBytes;
    const uint64_t addressSpace=uint64_t(1)<<32;
    if (!memory.data || memory.bytes>addressSpace-memory.baseAddress ||
        layout.address<memory.baseAddress || extent>UINT32_MAX)
        return fail("invalid original physical RAM window");
    const uint64_t offset=uint64_t(layout.address)-memory.baseAddress;
    if (offset>memory.bytes || extent>memory.bytes-offset)
        return fail("complete copy destination is outside original physical RAM window");
    layout.ramOffset=size_t(offset); layout.ramExtent=uint32_t(extent);
    return 0;
}
void packCopy(const CopyLayout& layout,const GxCopyTexels& input,unsigned char* data) noexcept {
    if (layout.format==15) {
        // Independently authored integer BT601 conversion. Signed chroma is
        // rounded before the original 1/4,1/2,1/4 horizontal neighbor filter.
        auto component=[](const unsigned char* p,unsigned channel) {
            const int r=p[0],g=p[1],b=p[2];
            const int value=channel==0 ? 66*r+129*g+25*b :
                channel==1 ? -38*r-74*g+112*b : 112*r-94*g-18*b;
            return (value+128)>>8;
        };
        for (uint32_t y=0;y<layout.rows;++y) for (uint32_t x=0;x<layout.width;x+=2) {
            const auto* row=input.data+size_t(y)*input.stride;
            const auto* a=row+x*4; const auto* b=a+4;
            const auto* previous=x ? a-4 : a;
            auto* pair=data+size_t(y)*layout.rowBytes+x*2;
            pair[0]=static_cast<unsigned char>(16+component(a,0));
            pair[2]=static_cast<unsigned char>(16+component(b,0));
            for (unsigned c=1;c<3;++c)
                pair[c==1 ? 1 : 3]=static_cast<unsigned char>(128+
                    ((component(previous,c)+2*component(a,c)+component(b,c))>>2));
        }
        return;
    }
    for (uint32_t by=0;by<layout.rows;++by) for (uint32_t bx=0;bx<layout.columns;++bx) {
        auto* tile=data+size_t(by)*layout.rowBytes+bx*layout.blockBytes;
        for (uint32_t y=0;y<layout.blockHeight;++y) for (uint32_t x=0;x<layout.blockWidth;++x) {
            const auto* p=input.data+size_t(by*layout.blockHeight+y)*input.stride+(bx*layout.blockWidth+x)*4;
            const uint32_t i=y*layout.blockWidth+x;
            switch (layout.format) {
            case 0:
                if (!(i&1)) tile[i/2]=p[0]&0xF0;
                else tile[i/2]|=p[0]>>4;
                break;
            case 1: case 8: tile[i]=p[0]; break;
            case 2: tile[i]=(p[3]&0xF0)|(p[0]>>4); break;
            case 3: tile[i*2]=p[3]; tile[i*2+1]=p[0]; break;
            case 4: {
                const uint32_t value=(uint32_t(p[0]>>3)<<11)|(uint32_t(p[1]>>2)<<5)|(p[2]>>3);
                tile[i*2]=value>>8; tile[i*2+1]=value; break;
            }
            case 5: {
                const uint32_t value=p[3]>=224 ?
                    0x8000|(uint32_t(p[0]>>3)<<10)|(uint32_t(p[1]>>3)<<5)|(p[2]>>3) :
                    (uint32_t(p[3]>>5)<<12)|(uint32_t(p[0]>>4)<<8)|(uint32_t(p[1]>>4)<<4)|(p[2]>>4);
                tile[i*2]=value>>8; tile[i*2+1]=value; break;
            }
            case 6:
                tile[i*2]=p[3]; tile[i*2+1]=p[0]; tile[32+i*2]=p[1]; tile[33+i*2]=p[2]; break;
            case 7: tile[i]=p[3]; break;
            case 9: tile[i]=p[1]; break;
            case 10: tile[i]=p[2]; break;
            case 11: tile[i*2]=p[1]; tile[i*2+1]=p[0]; break;
            case 12: tile[i*2]=p[2]; tile[i*2+1]=p[1]; break;
            }
        }
    }
}
}
struct GxCopyHistory {
    std::shared_ptr<GxCopyBytes> entries;
    uint64_t nextSequence=1,lastFrame=0;
    bool haveFrame=false,closing=false;

    int store(uint64_t frame,const SpanCopy& copy,const GxCopyTexels& input,const GxCopyMemory& memory,
                 std::shared_ptr<const GxCopyBytes>& out,std::string& error) {
        if (closing || out || (haveFrame && frame<lastFrame) || nextSequence==UINT64_MAX) {
            error="copy publication requires empty output, open owner and ordered frame"; return invalid;
        }
        for (const auto* entry=entries.get();entry;entry=entry->mNext.get())
            if (entry->mFrame==frame && entry->mCommand.words[0]==copy.words[0]) {
                error="frame/copy ID already committed; replay would repeat RAM writes"; return invalid;
            }
        CopyLayout layout{};
        int result=copyLayout(copy,input,memory,layout,error); if (result<0) return result;
        auto* storage=new(std::nothrow) CopyStorage;
        if (!storage) { error="copy control-block CPU OOM"; return noMemory; }
        auto entry=std::allocate_shared<GxCopyBytes>(CopyAllocator<GxCopyBytes>{storage});
        entry->mData.reset(new(std::nothrow) unsigned char[size_t(layout.rowBytes)*layout.rows]);
        if (!entry->mData) { error="copy byte-buffer CPU OOM"; return noMemory; }
        entry->mCommand=copy; entry->mFrame=frame; entry->mSequence=nextSequence;
        entry->mAddress=layout.address; entry->mWidth=layout.width; entry->mHeight=layout.height;
        entry->mFormat=layout.format; entry->mRowBytes=layout.rowBytes; entry->mRows=layout.rows;
        entry->mRamStride=layout.ramStride; entry->mRamExtent=layout.ramExtent;
        packCopy(layout,input,entry->mData.get());
        // All preparation precedes RAM mutation, even if producer input aliases
        // that RAM. Hardware block rows can overlap; later rows win.
        for (uint32_t row=0;row<layout.rows;++row)
            std::memcpy(memory.data+layout.ramOffset+size_t(row)*layout.ramStride,
                entry->mData.get()+size_t(row)*layout.rowBytes,layout.rowBytes);
        entry->mNext=entries; entries=entry; out=entry;
        ++nextSequence; lastFrame=frame; haveFrame=true;
        return 0;
    }
    int lookup(uint64_t frame,uint32_t id,uint32_t address,std::shared_ptr<const GxCopyBytes>& out,std::string& error) {
        if (closing || out) { error="copy lookup requires an open owner and empty output"; return invalid; }
        for (auto* entry=&entries;*entry;entry=&(*entry)->mNext)
            if ((*entry)->mFrame<=frame && (*entry)->mCommand.words[0]==id && (*entry)->mAddress==address) {
                out=*entry; return 0;
            }
        error="no retained exact copy ID/address at or before requesting frame"; return invalid;
    }
    int collect(uint64_t retiredThrough) {
        if (closing) return invalid;
        for (auto* entry=&entries;*entry;) {
            auto* current=entry->get(); bool overwritten=false;
            for (auto* newer=entries.get();newer!=current;newer=newer->mNext.get())
                if (newer->mAddress==current->mAddress && newer->isXfb()==current->isXfb()) { overwritten=true; break; }
            if (current->mFrame<=retiredThrough && current->mFrame!=lastFrame && entry->use_count()==1 && overwritten)
                *entry=std::move(current->mNext);
            else entry=&current->mNext;
        }
        return 0;
    }
    bool close() {
        closing=true;
        for (auto* entry=&entries;*entry;entry=&(*entry)->mNext) if (entry->use_count()!=1) return false;
        // Explicit iterative unlink avoids recursive destruction of a long
        // shared_ptr chain on the native render thread's bounded stack.
        while (entries) entries=std::move(entries->mNext);
        return true;
    }
};
int createGxCopyHistory(GxCopyHistory** out) {
    if (!out || *out) return invalid;
    auto* history=new(std::nothrow) GxCopyHistory;
    if (!history) return noMemory;
    *out=history; return 0;
}
int storeGxCopyBytes(GxCopyHistory* history,uint64_t frame,const SpanCopy& copy,const GxCopyTexels& input,
                      const GxCopyMemory& memory,std::shared_ptr<const GxCopyBytes>& out,std::string& error) {
    error.clear();
    if (!history) { error="missing copy history owner"; return invalid; }
    return history->store(frame,copy,input,memory,out,error);
}
int lookupGxCopyBytes(GxCopyHistory* history,uint64_t frame,uint32_t id,uint32_t address,
                       std::shared_ptr<const GxCopyBytes>& out,std::string& error) {
    error.clear();
    if (!history) { error="missing copy history owner"; return invalid; }
    return history->lookup(frame,id,address,out,error);
}
int collectGxCopyBytes(GxCopyHistory* history,uint64_t retiredThrough) {
    return history ? history->collect(retiredThrough) : invalid;
}
int releaseGxCopyHistory(GxCopyHistory** owner) {
    if (!owner) return invalid;
    if (!*owner) return 0;
    if (!(*owner)->close()) return invalid;
    delete *owner; *owner=nullptr; return 0;
}
}
