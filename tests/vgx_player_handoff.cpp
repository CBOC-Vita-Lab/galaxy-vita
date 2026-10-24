// Keep the actual publisher, player and sleep handshake; substitute only the FIFO parser.
#include "../platform/src/gx/vgx_galaxy.cpp"

namespace {
std::atomic<uint64_t> consumed{0};
uint64_t readBytes = 0;
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void CaughtUp() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (sPlDone.load(std::memory_order_acquire) != sPubState.load(std::memory_order_acquire)) {
        Require(std::chrono::steady_clock::now() < deadline, "published bytes eventually reach the real player");
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}
}

extern "C" int64_t port_host_time_ns() { return 0; }
extern "C" int port_vita_thread_class(int) { return 0; }
extern "C" int port_vita_pin_core(int) { return 0; }
// Compile this same production wake into LLVM IR to check its store-to-load ordering contract.
extern "C" void wake_ordering_contract() { WakePlayer(); }

namespace vgx {
uint32_t ProcessStream(const uint8_t* bytes, uint32_t count, bool) {
    const uint32_t complete = count / 8 * 8;
    for (uint32_t i = 0; i != complete; ++i)
        Require(bytes[i] == static_cast<uint8_t>((readBytes + i) % 251), "no bytes lost, reordered or overwritten across segments");
    readBytes += complete;
    consumed.store(readBytes, std::memory_order_release);
    return complete; // Deliberately retain incomplete packets for a later publish/segment.
}
}

int main() {
    static uint8_t segments[2][4096];
    port_vgx_stream_setup(segments[0], segments[1], sizeof(segments[0]));
    std::thread([] { PlayerMain(nullptr); }).detach();
    uint8_t* wr = segments[0];
    uint64_t sent = 0;
    unsigned rotations = 0, sleepingPublishes = 0;
    for (unsigned i = 0; i != 100000; ++i) {
        if (i % 1000 == 0) {
            CaughtUp();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!sPlSleeping.load()) Require(std::chrono::steady_clock::now() < deadline, "player can go idle");
            ++sleepingPublishes;
        }
        const size_t count = 3 + i % 17;
        for (size_t j = 0; j != count; ++j) *wr++ = static_cast<uint8_t>(sent++ % 251);
        const bool full = wr - sSegBase[sGmEpoch & 1] > static_cast<ptrdiff_t>(sizeof(segments[0]) - 32);
        const uint32_t before = sGmEpoch;
        // Test explicit wake, buffered/no-wake publishes and full-segment waits.
        wr = port_vgx_publish(wr, full ? 1 : (i % 7 == 0 || i % 1000 == 0 ? 2 : 0));
        rotations += sGmEpoch != before;
    }
    while (sent % 8) *wr++ = static_cast<uint8_t>(sent++ % 251);
    port_vgx_publish(wr, 1);
    CaughtUp();
    Require(consumed.load(std::memory_order_acquire) == sent, "all complete packets consumed before full-publish return");
    Require(rotations > 100 && sleepingPublishes == 100, "fixture exercises segment reuse and both sleep/publish orders");
    std::printf("PASS: 100000 real publishes, %u segment rotations, 100 sleeping publishes, %llu ordered bytes; stress is not a weak-memory proof\n",
                rotations, static_cast<unsigned long long>(sent));
    std::fflush(stdout);
    // PlayerMain is deliberately process-lifetime production code, with no test-only shutdown hook.
    std::_Exit(0);
}
