#pragma once
#include "port/settings_font.h"
#include <cassert>
#include <cstdlib>

struct FontAllocationProbe {
    unsigned calls=0, failAt=0;
    size_t live=0, peak=0;
    void* blocks[16]{};
    size_t sizes[16]{};
    port::settings::FontAllocator allocator() { return {this, Allocate, Release}; }
    static void* Allocate(void* context,size_t size) {
        auto& self=*static_cast<FontAllocationProbe*>(context);
        if(++self.calls==self.failAt)return nullptr;
        void* data=std::malloc(size);assert(data);
        for(unsigned i=0;i<16;++i)if(!self.blocks[i]) {
            self.blocks[i]=data;self.sizes[i]=size;self.live+=size;
            if(self.live>self.peak)self.peak=self.live;return data;
        }
        std::abort();
    }
    static void Release(void* context,void* data) {
        auto& self=*static_cast<FontAllocationProbe*>(context);
        for(unsigned i=0;i<16;++i)if(self.blocks[i]==data) {
            self.live-=self.sizes[i];self.blocks[i]=nullptr;std::free(data);return;
        }
        std::abort();
    }
};
