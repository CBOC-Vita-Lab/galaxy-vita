#include "port/settings_lifecycle.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
static std::mutex sMtx;
static std::condition_variable sCv;
static std::vector<int> sJobs;
static std::atomic<uint64_t> sPubState{5}, sPlDone{4};
static std::atomic<bool> sPlayerOn{true};
static port::settings::HoldGate sSettingsRenderHold;
static std::atomic<unsigned> finishes{0}, resumes{0}, flushes{0};
static std::atomic<bool> finishBlocked{false};
static int64_t port_host_time_ns() { return std::chrono::steady_clock::now().time_since_epoch().count(); }
static void __PortGXFifoFlush() { ++flushes; }
static void WakePlayer() {}
static int port_vgx_vita_settings_finish(unsigned) {
    ++finishes;
    while(finishBlocked.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return 1;
}
static void port_vgx_vita_settings_resume() { ++resumes; }
#include "settings_render_actual.inc"
int main() {
    std::atomic<bool> stop{false};
    std::thread owner([&] {
        while(!stop.load()) {
            std::unique_lock<std::mutex> lock(sMtx);
            ServiceSettingsHold(lock);
            sCv.wait_for(lock,std::chrono::milliseconds(1));
        }
    });
    std::atomic<int> result{-1};
    std::thread guest([&] { result=port_vgx_settings_hold(1,200); });
    while(!flushes.load())std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(finishes.load()==0 && result.load()==-1);
    { std::lock_guard<std::mutex> lock(sMtx); sJobs.push_back(1); }
    sPlDone=5;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    assert(finishes.load()==0);
    { std::lock_guard<std::mutex> lock(sMtx); sJobs.clear(); }
    sCv.notify_all();guest.join();assert(result==1);
    assert(resumes==0);
    port_vgx_settings_release(1);
    while(resumes==0)std::this_thread::yield();
    finishBlocked=true;
    result=-1;
    std::thread timed([&] {result=port_vgx_settings_hold(2,15);});
    while(finishes<2)std::this_thread::yield();
    timed.join();assert(result==0);
    finishBlocked=false;
    while(resumes<2)std::this_thread::yield();
    assert(sSettingsRenderHold.Requested()==0);
    assert(port_vgx_settings_hold(3,200)==1);
    port_vgx_settings_release(2);assert(sSettingsRenderHold.Acknowledged(3));
    port_vgx_settings_release(3);
    stop=true;sCv.notify_all();owner.join();
    assert(resumes==3);
    std::puts("PASS: actual render hold drains publication/jobs; cancellation resumes late GPU completion");
}
