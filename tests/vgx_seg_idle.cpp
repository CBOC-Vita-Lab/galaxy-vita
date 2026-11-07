// GXV_SEG_IDLE (platform/src/gx/vgx_galaxy.cpp, port_vgx_publish flag 4): the staging segment changes at the game's idle points.
// The real publisher, player thread and sleep handshake (as tests/vgx_player_handoff.cpp); only the FIFO parser is substituted: it checks
// that every byte arrives once, in order, whatever segment it was written to or copied into, and keeps incomplete packets back.
//   Phase 1: idle points made right after the player caught up: every segment change is made there, none after a full segment.
//   Phase 2: idle points at random moments, the player busy or not (a slow parser): bytes stay ordered, full waits still work.
//   Phase 3: GXV_SEG_IDLE's limit: no change at an idle point while the segment holds less than it.
// Host build (g++; ThreadSanitizer optional):
//   g++ -std=c++20 -O1 -g -DTARGET_PC=1 -DNDEBUG=1 -DPORT_GX_VGX=1 -DPORT_GX_VGX_ONLY=1 -DPORT_RUNTIME_DIAGNOSTICS=0 -DPORT_LOGGING=0 \
//     -ffunction-sections -fdata-sections -Wl,--gc-sections -Iplatform/include -Ivita/runtime -Iplatform/vgx/include -Iplatform/vgx/src \
//     -Idecomp/libs/RVL_SDK/include -Wno-register -Wno-unknown-pragmas -Wno-deprecated-declarations tests/vgx_seg_idle.cpp -pthread \
//     [-fsanitize=thread] -o /tmp/seg_idle && /tmp/seg_idle
#include <cstdlib>
#include "../platform/src/gx/vgx_galaxy.cpp"

namespace {
std::atomic<uint64_t> consumed{0};
std::atomic<int> slowParse{0};
uint64_t readBytes = 0;
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void CaughtUp() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (sPlDone.load(std::memory_order_acquire) != sPubState.load(std::memory_order_acquire)) {
        Require(std::chrono::steady_clock::now() < deadline, "published bytes eventually reach the real player");
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }
}
}

extern "C" int64_t port_host_time_ns() { return 1; }
extern "C" int port_vita_thread_class(int) { return 0; }
extern "C" int port_vita_pin_core(int) { return 0; }

namespace vgx {
uint32_t ProcessStream(const uint8_t* bytes, uint32_t count, bool) {
    const uint32_t complete = count / 8 * 8;
    for (uint32_t i = 0; i != complete; ++i)
        Require(bytes[i] == static_cast<uint8_t>((readBytes + i) % 251), "no bytes lost, reordered or overwritten across segments");
    readBytes += complete;
    if (slowParse.load(std::memory_order_relaxed)) std::this_thread::sleep_for(std::chrono::microseconds(complete % 3 == 0 ? 30 : 1));
    consumed.store(readBytes, std::memory_order_release);
    return complete; // (an incomplete packet stays for a later publish / segment)
}
}

int main() {
    setenv("GXV_SEG_IDLE", "1", 1); // (1 KiB: the segments of this test are 4 KiB)
    static uint8_t segments[2][4096];
    port_vgx_stream_setup(segments[0], segments[1], sizeof(segments[0]));
    std::thread([] { PlayerMain(nullptr); }).detach();
    uint8_t* wr = segments[0];
    uint64_t sent = 0;
    auto write = [&](size_t count) { for (size_t j = 0; j != count; ++j) *wr++ = static_cast<uint8_t>(sent++ % 251); };
    auto fullNow = [&] { return wr - sSegBase[sGmEpoch & 1] > static_cast<ptrdiff_t>(sizeof(segments[0]) - 64); };
    unsigned idleChanges = 0, otherChanges = 0;
    // ---- phase 1: an idle point after each "picture" (the player has parsed everything published)
    for (unsigned pic = 0; pic != 3000; ++pic) {
        for (unsigned k = 0; k != 6; ++k) {
            write(3 + (pic * 7 + k) % 61);
            const uint32_t before = sGmEpoch;
            wr = port_vgx_publish(wr, fullNow() ? 1 : (k == 5 ? 2 : 0));
            otherChanges += sGmEpoch != before;
        }
        write(pic % 5); // (bytes not published yet at the idle point: they go along to the other segment)
        CaughtUp();
        const uint32_t before = sGmEpoch;
        wr = port_vgx_publish(wr, 4);
        idleChanges += sGmEpoch != before;
    }
    Require(idleChanges > 300, "phase 1: segments change at idle points");
    Require(otherChanges == 0 && sSeg.full == 0, "phase 1: no segment fills up while the game has idle points after the player caught up");
    Require(sSeg.idle == idleChanges, "phase 1: counted");
    // ---- phase 2: idle points at any moment, a slow parser: changes only while the player is idle, ordering kept, full waits still work
    slowParse.store(1);
    unsigned idle2 = 0, full2 = 0;
    for (unsigned i = 0; i != 40000; ++i) {
        write(3 + i % 17);
        if (i % 61 == 0) std::this_thread::sleep_for(std::chrono::microseconds(300)); // (now and then the player catches up)
        const uint32_t before = sGmEpoch;
        const bool full = fullNow();
        const bool idle = !full && i % 5 == 0;
        wr = port_vgx_publish(wr, full ? 1 : idle ? (i % 10 == 0 ? 6 : 4) : (i % 7 == 0 ? 2 : 0));
        if (sGmEpoch != before) (idle ? idle2 : full2) += 1;
    }
    slowParse.store(0);
    Require(idle2 > 50, "phase 2: idle-point changes happen while the parser keeps up now and then");
    Require(full2 > 0, "phase 2: the full-segment path still changes segments when the player is behind");
    // ---- phase 3: below the limit, no change at an idle point
    write(8);
    wr = port_vgx_publish(wr, 2);
    CaughtUp();
    {
        const uint32_t before = sGmEpoch;
        const size_t used = static_cast<size_t>(wr - sSegBase[sGmEpoch & 1]);
        wr = port_vgx_publish(wr, 4);
        Require((used >= 1024) == (sGmEpoch != before), "phase 3: an idle point changes segments exactly when at least GXV_SEG_IDLE is used");
    }
    // ---- the end: everything complete arrives
    while (sent % 8) write(1);
    wr = port_vgx_publish(wr, 2);
    CaughtUp();
    Require(consumed.load(std::memory_order_acquire) == sent, "all complete packets parsed, once and in order");
    std::printf("PASS: seg-idle: phase 1 %u changes at idle points, 0 full; phase 2 %u at idle points, %u after full waits; %llu ordered bytes\n",
                idleChanges, idle2, full2, static_cast<unsigned long long>(sent));
    std::fflush(stdout);
    std::_Exit(0);
}
