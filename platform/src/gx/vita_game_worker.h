#pragma once
#include "gx_live.h"
#include "vita_material_cache.h"

namespace gpu {
// One host thread owns creation, submission and retirement of every GXM owner.
// Requests block the original guest CPU; no guest scheduler call or RAM window
// survives acknowledgement. A native failure poisons further game requests.
class VitaGameWorker final : public RecorderConsumer {
public:
    ~VitaGameWorker() override;
    bool consume(std::shared_ptr<const Frame>,const RecorderMemory&,std::string&) override;
    bool peek(const RecorderPeek&,uint32_t&,std::string&) override;
    bool present(const void*,bool,std::string&) override;
    // Stop guest producers first. Failed native retirement retains the thread
    // and all remaining native ownership; only cleanup may be retried.
    bool close(std::string& error);
private:
    struct State;
    std::unique_ptr<State> state;
    explicit VitaGameWorker(VitaShaderLibrary);
    friend bool createVitaGameWorker(VitaShaderLibrary,std::shared_ptr<VitaGameWorker>&,std::string&);
};
// Attach returned consumer before port_boot. On initialization failure output
// may own partial native resources; call close, do not silently lose ownership.
bool createVitaGameWorker(VitaShaderLibrary,std::shared_ptr<VitaGameWorker>&,std::string&);
}
