// The actual scheduler/IRQ implementation, not a second model of its loop.
// Include permits isolated safe-point setup without a test-only production API.
#include "../platform/src/os/os_thread.cpp"
#include <cstdio>
#include <cstdlib>

extern "C" void port_log(const char*,...) {}
extern "C" int64_t port_host_time_ns() { return 0; }
extern "C" void port_fatal(const char*,...) { std::abort(); }
extern "C" void* port_low_alloc(size_t) { std::abort(); }
extern "C" void __OSUnlockAllMutex(OSThread*) { std::abort(); }

static unsigned viCalls,alarmCalls,guestProgress,checks;
static void require(bool value,const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr,"IRQ scheduler FAILED: %s\n",message); std::exit(1); }
}
static void video() {
    require(sInIrq && !sIntEnabled,"original handler runs on the virtual CPU with interrupts masked");
    ++viCalls;
    require(viCalls<=guestProgress+1,
        "a VI arriving during a slow successful display cannot monopolize the IRQ drain before guest progress");
    // A display taking at least one retrace leaves another VI bit pending.
    // No artificial timing race: exercise that exact atomic producer boundary.
    port_irq_raise(PORT_IRQ_VI); port_irq_raise(PORT_IRQ_ALARM);
    RunQueueHint=TRUE;
}
static void alarm() { ++alarmCalls; }
int main() {
    tlsSelf=&DefaultThread; __OSCurrentThread=&DefaultThread; sCpuOwner=&DefaultThread;
    DefaultThread.state=OS_THREAD_STATE_RUNNING; DefaultThread.priority=0;
    sIntEnabled=true; port_irq_set_handler(PORT_IRQ_VI,video); port_irq_set_handler(PORT_IRQ_ALARM,alarm);
    port_irq_raise(PORT_IRQ_VI);
    for (unsigned i=0;i<64;++i) {
        port_irq_poll();
        require(viCalls==i+1 && alarmCalls==i && sIntEnabled && !sInIrq,
            "one bounded IRQ snapshot returns to guest, retaining later VI and alarm work for the next safe point");
        ++guestProgress;
    }
    port_irq_set_handler(PORT_IRQ_VI,nullptr); port_irq_poll();
    require(alarmCalls==64 && !sPendingIrq.load(),"deferred original completion is eventually delivered, not lost");
    const BOOL enabled=OSDisableInterrupts(); port_irq_raise(PORT_IRQ_ALARM); port_irq_poll();
    require(enabled && alarmCalls==64,"masked original CPU does not prematurely deliver an IRQ");
    OSRestoreInterrupts(enabled);
    require(alarmCalls==65,"reenabling original interrupts delivers the retained completion once");
    std::printf("Original IRQ scheduler: %u real bounded-delivery/progress checks; not native timing proof\n",checks);
}
