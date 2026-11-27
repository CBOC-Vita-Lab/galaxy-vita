#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include "../armhf/glue/vclock.cpp"
extern "C" void port_log(const char*, ...) {}
extern "C" void port_host_sleep_ns(int64_t) { std::abort(); }
extern "C" int port_vgx_active() { return 1; }
namespace {
std::mutex sMtx;
std::condition_variable sCv;
std::string sCapPath;
bool sCapDone = false, sCapWanted = false;
unsigned long long sCapFrame = 0;
}
#include "capture_function.inc"
extern "C" unsigned long long port_headless_render(const char*, int);
static void Complete(unsigned long long shown, bool cap) {
    {
        std::unique_lock<std::mutex> lk(sMtx);
#include "capture_completion.inc"
    }
    sCv.notify_all();
}
static void Check(unsigned long long frame, bool complete) {
    {
        std::unique_lock<std::mutex> lk(sMtx);
        sCapDone = true; sCapWanted = false; sCapFrame = 999999;
    }
    std::atomic<bool> stop{false};
    bool requested = false;
    std::thread render([&] {
        while (!stop.load()) {
            {
                std::unique_lock<std::mutex> lk(sMtx);
                if (sCapWanted) { requested = true; break; }
            }
            std::this_thread::yield();
        }
        if (requested) {
            { std::lock_guard<std::mutex> clockLock(sM); assert(sOutstanding == 0); }
            Complete(frame, complete);
        }
    });
    const auto start = std::chrono::steady_clock::now();
    const auto returned = port_headless_render("no-pixel-output.png", 1);
    stop.store(true); render.join();
    assert(requested && returned == (complete ? frame : 0));
    assert(sCapPath == "no-pixel-output.png" && !sCapWanted);
    if (!complete) {
        assert(!sCapDone);
        assert(std::chrono::steady_clock::now() - start >= std::chrono::seconds(4));
    }
}
int main() {
    setenv("PETARI_VCLOCK", "1", 1);
    // The real ARMHF desktop entry names host-main before any capture, which
    // constructs clock TLS before release takes the clock lock.
    std::thread named([] {
        port_vclock_name_thread("host-main");
        port_vclock_release();
    });
    named.join();
    port_vclock_name_thread("host-main");
    // Wake the actual calling thread through the real virtual-clock machinery.
    // It owes settle until the ARMHF capture bridge releases it before blocking.
    std::thread clock([] {
        for (;;) {
            { std::lock_guard<std::mutex> lk(sM); if (!sQueue.empty()) break; }
            std::this_thread::yield();
        }
        port_vclock_idle_step([] { return 0; });
    });
    port_vclock_sleep_ns(1); clock.join();
    { std::lock_guard<std::mutex> lk(sM); assert(sOutstanding == 1 && tlsSleeper.owesSettle); }
    Check(42, true);
    Check(0, true);
    Check(77, false); // A notification without completed work cannot bypass the timeout.
    puts("PASS actual virtual-clock wake/settle and capture handshake: frame42, frame0, spurious notify/timeout0; no pixel proof");
}
