// Link the unmodified production translation unit, without scheduling hooks.
#include "vgx_guestmem.h"
#include "vgx_hle.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
alignas(4096) unsigned char ram[16384] = {};
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::abort(); }
}
}
int main() {
    vgx::hle::RegisterGuestRam(ram, sizeof ram);
    constexpr unsigned writers = 3, turns = 20000;
    std::atomic<bool> go{false};
    std::atomic<unsigned> done{0};
    std::thread threads[writers];
    const uint64_t first = vgx::GuestWriteEpoch();
    const uint64_t generation = vgx::GuestWriteGeneration(ram, sizeof ram);
    for (unsigned i = 0; i < writers; ++i) threads[i] = std::thread([&, i] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (unsigned j = 0; j < turns; ++j) vgx::hle::NotifyGuestWrite(ram + 4096 * i, 4);
        done.fetch_add(1, std::memory_order_release);
    });
    go.store(true, std::memory_order_release);
    const vgx::HostRange all{ram, sizeof ram}, unused{ram + 12288, 4096};
    unsigned checks = 0, conservative = 0;
    while (done.load(std::memory_order_acquire) != writers) {
        const uint64_t end = vgx::GuestWriteEpoch();
        if (end > first) {
            Require(vgx::GuestWritesTouch(end - 1, end, &all, 1), "concurrent record must include RAM or reject");
            conservative += vgx::GuestWritesTouch(end - 1, end, &unused, 1);
            ++checks;
        }
    }
    for (auto& thread : threads) thread.join();
    Require(vgx::GuestWriteEpoch() == first + writers * turns, "all notifications completed");
    Require(vgx::GuestWriteGeneration(ram, sizeof ram) == generation + writers * turns, "no page counter increments lost");
    const uint64_t tail = vgx::GuestWriteEpoch();
    for (unsigned i = 0; i < 128; ++i) vgx::hle::NotifyGuestWrite(ram + 4096, 4);
    const vgx::HostRange touched{ram + 4096, 4}, other{ram, 4096};
    Require(!vgx::GuestWritesTouch(tail, tail + 128, &unused, 1), "quiescent tail precisely excludes unused page");
    Require(!vgx::GuestWritesTouch(tail, tail + 128, &other, 1), "quiescent tail precisely excludes other written page");
    Require(vgx::GuestWritesTouch(tail, tail + 128, &touched, 1), "quiescent tail includes touched page");
    vgx::hle::NotifyGuestWrite(ram, 0);
    vgx::hle::NotifyGuestWrite(ram + sizeof ram, 1);
    Require(vgx::GuestWriteEpoch() == tail + 128, "empty and untracked writes do not advance epoch");
    Require(vgx::GuestWriteGeneration(ram + sizeof ram, 1) == vgx::kNotGuest, "out-of-bank range is untracked");
    std::printf("PASS: unhooked production concurrency: %u notifications, %u live checks, %u conservative reads, precise128-record tail\n",
                writers * turns, checks, conservative);
}
