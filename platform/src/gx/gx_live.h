#pragma once
#include "gx_record.h"
#include <string>

namespace gpu {
struct RecorderMemory {
    const unsigned char* tlut=nullptr;
    size_t tlutBytes=0;
};
struct RecorderPeek {
    bool depth=false;
    uint16_t x=0,y=0;
    uint32_t pixelFormat=0,alphaRead=1; // BP 0x43 and CPU PE register4 AFMT
};

// Original command-time delivery, never a latest-frame notification. The caller
// retains the guest CPU token while blocked in these methods. A host GPU worker
// must finish the request before returning and must not call guest OS/GX APIs.
// RAM and this TLUT window are stable only until return; retain immutable Frame
// images/commands, not these live pointers. Carry terminal EFB dimensions between
// segments when building spans. A successful copy means RAM is already visible;
// a successful draw/peek means preceding native work is fenced. Errors are fatal
// to the game stream: no later command, automatic retry or fabricated result.
class RecorderConsumer {
public:
    virtual ~RecorderConsumer() = default;
    virtual bool consume(std::shared_ptr<const Frame>,const RecorderMemory&,std::string& error) = 0;
    virtual bool peek(const RecorderPeek&,uint32_t& result,std::string& error) = 0;
    virtual bool present(const void* xfb,bool black,std::string& error) = 0;
};
// One consumer for the game session, owned until process teardown. Attach only
// before recorded work; replacing a live consumer or dropping pending work fails.
bool attachRecorderConsumer(std::shared_ptr<RecorderConsumer>,std::string& error);
}
