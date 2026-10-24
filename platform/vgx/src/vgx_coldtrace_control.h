#pragma once
#include "vgx_coldtrace.h"

#if VGX_COLDTRACE
namespace vgx::coldtrace {
enum class ControlState : uint32_t { Dormant,Recording,FreezeRequested,Draining,Writing,Complete,Failed };
constexpr int kControlScopeMisuse=-10001,kControlNoExporter=-10002;
class Control {
public:
    using Export=int(*)(const FrozenView&,void*) noexcept;
    constexpr explicit Control(Recorder& recorder) noexcept:recorder_(recorder){}
    // Single-shot, after successful Arm. No rearm while a report reader can exist.
    bool Activate() noexcept {
        auto expected=ControlState::Dormant;
        return state_.compare_exchange_strong(expected,ControlState::Recording,std::memory_order_release,std::memory_order_relaxed);
    }
    // Render owner, outside all item/leaf scopes. Never waits for worker hooks.
    void RenderBoundary() noexcept {
        auto expected=ControlState::FreezeRequested;
        if(state_.compare_exchange_strong(expected,ControlState::Draining,std::memory_order_acq_rel,std::memory_order_relaxed))recorder_.RequestFreeze();
    }
    // Game-thread pause-menu turn only. Unpausing defers export; claiming it never blocks on drain.
    void PauseTurn(bool selecting,Export writer,void* context) noexcept {
        if(!selecting)return;
        auto state=state_.load(std::memory_order_acquire);
        if(state==ControlState::Recording){state_.compare_exchange_strong(state,ControlState::FreezeRequested,std::memory_order_acq_rel,std::memory_order_relaxed);return;}
        if(state!=ControlState::Draining)return;
        const bool misused=recorder_.ScopeMisuses()!=0;
        const auto view=recorder_.Frozen();
        if(!misused && !view.data)return;
        if(!state_.compare_exchange_strong(state,ControlState::Writing,std::memory_order_acq_rel,std::memory_order_relaxed))return;
        const int result=misused?kControlScopeMisuse:(writer?writer(view,context):kControlNoExporter);
        error_.store(result,std::memory_order_release);
        state_.store(result==0?ControlState::Complete:ControlState::Failed,std::memory_order_release);
    }
    ControlState Status() const noexcept {return state_.load(std::memory_order_acquire);}
    int Error() const noexcept {return error_.load(std::memory_order_acquire);}
private:
    Recorder& recorder_;
    std::atomic<ControlState> state_{ControlState::Dormant};
    std::atomic<int> error_{0};
};
static_assert(std::atomic<ControlState>::is_always_lock_free && std::atomic<int>::is_always_lock_free);
} // namespace vgx::coldtrace
#endif
