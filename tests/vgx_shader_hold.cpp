#include <atomic>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <string>
#include "../platform/vgx/vita/vgx_boot_watch.h"
namespace vgx::backend {
unsigned pending = 3, total = 10;
bool yielded = false;
double budget = 0;
unsigned WarmPending() { return pending; }
unsigned WarmTotal() { return total; }
bool WarmCapacityYielded() { return yielded; }
void SetWarmBudget(double b) { budget = b; }
}
namespace splash {
enum { kOff, kGl, kYielded };
std::atomic<int> g_phase{kGl};
std::atomic<bool> g_failed{false}, g_titleSeen{false};
std::atomic<uint64_t> g_titleSeenUs{0};
bool g_heldForShaders = false, g_warmAfterTitle = false;
uint64_t g_holdSinceUs = 0, clockUs = 1000000;
unsigned tasks = 0;
uint64_t NowUs() { return clockUs; }
unsigned NowMs() { return unsigned(clockUs/1000); }
long ReadSeconds(const char* name, long d) { const auto e = std::getenv(name); return e ? std::strtol(e,nullptr,10) : d; }
void EnvLong(const char*, long&, long, long) {}
template<class... T> void port_log(const char*, T...) {}
#define GXV_BASIC_LOG(...) port_log(__VA_ARGS__)
void TaskProgress(const char*, int, int, bool = true) { ++tasks; }
#include "hold_function.inc"
void Notify(bool genuine) {
    NoteTitleVisible("actual-source fixture", genuine);

}
}
int main(int argc,char** argv) {
    assert(argc==2); const std::string mode=argv[1];
    const bool capacity=mode.find("capacity")!=std::string::npos;
    const bool unlimited=mode.find("unlimited")!=std::string::npos;
    const bool off=mode=="hold-off";
    const bool genuine=mode.find("fallback")==std::string::npos;
    setenv("GXV_WARM_HOLD",off?"0":"1",1);
    setenv("GXV_WARM_HOLD_MAX_SEC",unlimited?"0":"90",1);
    vgx::backend::yielded=capacity;
    splash::Notify(genuine);
    if(off||(capacity&&genuine)) {
        if(!splash::g_titleSeen.load()) { std::fprintf(stderr,"FAIL %s: real title must complete without pretending pending units retired\n",argv[1]); return 1; }
        assert(vgx::backend::pending==3 && vgx::backend::total==10);
    } else {
        assert(!splash::g_titleSeen.load()); splash::clockUs+=100000000;
        splash::Notify(genuine);
        assert(splash::g_titleSeen.load()==!unlimited);
    }
    std::printf("PASS actual NoteTitleVisible mode=%s title_seen=%d pending=%u\n",argv[1],splash::g_titleSeen.load(),vgx::backend::pending);
}
