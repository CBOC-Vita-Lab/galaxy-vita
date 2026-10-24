// The splash ticker publishes the budget while the render thread consumes it.
extern "C" { unsigned char vgxvita_maskGuardDue = 0; }
namespace vgx::backend { namespace { __attribute__((noinline)) void RunWarm(); } }
#include "../platform/vgx/src/vgx_backend_gl.cpp"
#include <thread>

extern "C" __attribute__((noinline)) double read_warm_budget_probe() {
    return vgx::backend::g_warmBudgetMs.load(std::memory_order_relaxed);
}
int main() {
    std::atomic<bool> start{false};
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (unsigned i = 0; i < 100000; ++i) vgx::backend::SetWarmBudget(i & 1 ? 10.0 : 0.0);
    });
    start.store(true, std::memory_order_release);
    for (unsigned i = 0; i < 100000; ++i) {
        const double value = read_warm_budget_probe();
        if (value != 25.0 && value != 10.0 && value != 0.0) std::abort();
    }
    writer.join();
    std::puts("PASS: concurrent real warm-budget setter/consumer; stress alone is not a race proof");
}
