#include <cassert>
#include <cstdio>
#include <chrono>
#include <vector>
namespace vgx::backend {
bool capacity;
unsigned idle;
unsigned WarmPending() { return 3; }
bool WarmCapacityYielded() { return capacity; }
void WarmIdle() { ++idle; }
}
int active, services;
bool job;
struct Jobs { bool empty() { return !job; } } sJobs;
struct Lock { void lock() {} void unlock() {} } lk;
struct Cv {
    std::vector<int> waits;
    template<class F> bool wait_for(Lock&, std::chrono::milliseconds d, F f) {
        waits.push_back(int(d.count()));
        if (waits.size() == 3) job = true;
        return f();
    }
} sCv;
void ServiceSettingsHold(Lock&) {}
int port_vgx_vita_splash_active() { return active; }
void port_vgx_vita_idle_gap(unsigned) { ++services; }
void port_vgx_vita_idle() { ++services; }
void ActualIdleLoop() {
    const bool warmIdleOn = true;
#include "idle_loop.inc"
}
int main() {
    for (active = 0; active <= 1; ++active) for (int blocked = 0; blocked <= 1; ++blocked) {
        job = false; services = 0; vgx::backend::idle = 0;
        vgx::backend::capacity = blocked; sCv.waits.clear(); ActualIdleLoop();
        for (auto ms : sCv.waits) assert(ms == (blocked ? 20 : 1));
        assert(vgx::backend::idle == unsigned(blocked ? 0 : 2));
        assert(services == (active ? 2 : 0));
    }
    puts("PASS actual idle loop: capacity yield waits20ms, zeroWarmIdle; startup remains serviced");
}
