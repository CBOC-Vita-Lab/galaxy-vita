#include <cstdio>
#if __has_include("../platform/vgx/src/vgx_coldtrace_control.h")
#include "../platform/vgx/src/vgx_coldtrace_control.h"
#include <atomic>
#include <cstdlib>
#include <optional>
#include <thread>

namespace {
using namespace vgx::coldtrace;
std::atomic<uint64_t> now{0};
thread_local uint32_t tid=1;
uint64_t Clock() noexcept {return now.load();}
uint32_t Thread() noexcept {return tid;}
void Require(bool ok,const char* text){if(!ok){std::fprintf(stderr,"FAIL: %s\n",text);std::exit(1);}}
struct Export {
    std::atomic<unsigned> calls{0};bool success=true;int error=-17;
    static int Run(const FrozenView& view,void* pointer) noexcept {
        auto& self=*static_cast<Export*>(pointer);
        Require(view.data && view.data->version==VGX_COLDTRACE_DETAIL_VERSION,"export receives only stable frozen data with the current recorder layout");
        self.calls.fetch_add(1);return self.success?0:self.error;
    }
};
void OneShotAndUnpause() {
    Recorder recorder;Control control(recorder);Export sink;
    control.PauseTurn(true,Export::Run,&sink);control.RenderBoundary();
    Require(control.Status()==ControlState::Dormant && sink.calls==0,"unarmed pause never creates a report");
    Require(recorder.Arm(Clock,Thread,1) && control.Activate(),"activate only after recorder Arm");
    Require(!control.Activate(),"activation is single-shot");
    for(unsigned i=0;i<1000;++i)control.RenderBoundary();
    Require(control.Status()==ControlState::Recording && !recorder.Frozen().data,"ordinary render boundaries cannot freeze a recording without a pause request");
    control.PauseTurn(false,Export::Run,&sink);Require(control.Status()==ControlState::Recording,"ordinary play does not freeze or export");
    {Scope root(recorder,Key{3,4,5,6,7,8});now.store(208000);}
    control.PauseTurn(true,Export::Run,&sink);
    Require(control.Status()==ControlState::FreezeRequested && sink.calls==0 && !recorder.Frozen().data,"pause requests freeze but cannot consume a live capture");
    control.PauseTurn(false,Export::Run,&sink);
    control.RenderBoundary();Require(control.Status()==ControlState::Draining && recorder.Frozen().data,"renderer closes admissions at the explicit boundary");
    control.PauseTurn(false,Export::Run,&sink);Require(sink.calls==0,"unpausing before export defers all writes");
    control.PauseTurn(true,Export::Run,&sink);
    Require(control.Status()==ControlState::Complete && sink.calls==1 && control.Error()==0,"next paused turn exports exactly once");
    for(unsigned i=0;i<1000;++i){control.PauseTurn(true,Export::Run,&sink);control.RenderBoundary();}
    Require(sink.calls==1 && !control.Activate(),"later menus do not duplicate the report or rearm");
    const auto view=recorder.Frozen();Require(view.data->itemsSeen==1 && !(view.data->worst[0].flags&kFrozenMidItem),"a boundary freeze leaves completed warm items complete");
    std::puts("PASS: pause requests, render-boundary freeze, unpause deferral and one-shot export");
}
void DrainAndFailure() {
    Recorder recorder;Control control(recorder);Export sink;sink.success=false;
    Require(recorder.Arm(Clock,Thread,1) && control.Activate(),"activate blocked-worker case");
    std::atomic<bool> started{false},release{false};
    std::thread worker([&]{tid=2;Scope read(recorder,Phase::Read,IoClass::Read);started.store(true);
        while(!release.load())std::this_thread::yield();read.Result(7,9);});
    while(!started.load())std::this_thread::yield();
    control.PauseTurn(true,Export::Run,&sink);control.RenderBoundary();
    for(unsigned i=0;i<1000;++i)control.PauseTurn(true,Export::Run,&sink);
    Require(control.Status()==ControlState::Draining && sink.calls==0 && recorder.PendingPermits()==1,"pending worker hooks defer export without waiting or opening files");
    release.store(true);worker.join();control.PauseTurn(true,Export::Run,&sink);
    Require(control.Status()==ControlState::Failed && control.Error()==-17 && sink.calls==1,"export failure is explicit and not retried each frame");
    control.PauseTurn(true,Export::Run,&sink);Require(sink.calls==1,"failed report is not silently duplicated");
    std::puts("PASS: blocked-I/O drain and export failure are non-spinning and terminal");
}
void CompetingConsumersAndMisuse() {
    Recorder recorder;Control control(recorder);Export sink;
    Require(recorder.Arm(Clock,Thread,1) && control.Activate(),"activate competing report readers");
    control.PauseTurn(true,Export::Run,&sink);control.RenderBoundary();
    std::thread first([&]{control.PauseTurn(true,Export::Run,&sink);});
    std::thread second([&]{control.PauseTurn(true,Export::Run,&sink);});first.join();second.join();
    Require(control.Status()==ControlState::Complete && sink.calls==1,"only one consumer can claim export");
    Recorder broken;Control bad(broken);Export never;
    Require(broken.Arm(Clock,Thread,1) && bad.Activate(),"activate misuse reporting");
    std::optional<Scope> scope;scope.emplace(broken,Key{});
    std::thread wrong([&]{tid=2;scope.reset();});wrong.join();
    bad.PauseTurn(true,Export::Run,&never);bad.RenderBoundary();bad.PauseTurn(true,Export::Run,&never);
    Require(bad.Status()==ControlState::Failed && bad.Error()==kControlScopeMisuse && never.calls==0,"a leaked misuse permit produces an explicit failure, not a fake report");
    std::puts("PASS: competing consumers and permanent scope misuse are fail-closed");
}
void ConcurrentDrainStress() {
    for(unsigned round=0;round<64;++round){
        Recorder recorder;Control control(recorder);Export sink;Require(recorder.Arm(Clock,Thread,1) && control.Activate(),"activate concurrent boundary/pause stress");
        std::atomic<bool> started{false},release{false};
        std::thread worker([&]{tid=2;Scope read(recorder,Phase::Read,IoClass::Read);started.store(true);while(!release.load())std::this_thread::yield();read.Result(1,1);});
        while(!started.load())std::this_thread::yield();
        const auto terminal=[&]{const auto state=control.Status();return state==ControlState::Complete || state==ControlState::Failed;};
        std::thread render([&]{while(!terminal()){control.RenderBoundary();std::this_thread::yield();}});
        std::thread game([&]{while(!terminal()){control.PauseTurn(true,Export::Run,&sink);std::this_thread::yield();}});
        release.store(true);worker.join();render.join();game.join();Require(control.Status()==ControlState::Complete && sink.calls==1,"concurrent drain emits one report");
    }
    std::puts("PASS: 64 concurrent renderer/pause/blocked-worker drain rounds");
}
}
int main(){OneShotAndUnpause();DrainAndFailure();CompetingConsumersAndMisuse();ConcurrentDrainStress();}
#else
int main(){std::fputs("FAIL: pause-boundary coldtrace control is not implemented\n",stderr);return 1;}
#endif
