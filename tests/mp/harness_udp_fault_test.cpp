// Host test of the Same Wi-Fi UDP link's test fault injector (net/src/vlink_udp.cpp, SMG_NET_DROP / DUP / DELAY_MS).
// Two UDP backends on 127.0.0.1; A sends 2000 datagrams to B (broadcast to its target list), B counts what arrives.
//   g++ -std=c++20 -g -O1 -fsanitize=address,undefined (this box: clang++ has no ASan runtime)
//     -Inet/src -Inet/include tests/mp/harness_udp_fault_test.cpp
//     net/src/vlink.cpp net/src/vlink_udp.cpp -o /tmp/udp_fault_test -lpthread && /tmp/udp_fault_test
#include "vlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void sleepMs(int ms) { timespec ts{0, ms * 1000000L}; nanosleep(&ts, nullptr); }

static int run(const char* drop, const char* dup, const char* delay, int* outOrderFlips) {
    unsetenv("SMG_NET_DROP"); unsetenv("SMG_NET_DUP"); unsetenv("SMG_NET_DELAY_MS"); unsetenv("SMG_NET_JITTER_MS");
    if (drop) setenv("SMG_NET_DROP", drop, 1);
    if (dup) setenv("SMG_NET_DUP", dup, 1);
    if (delay) setenv("SMG_NET_DELAY_MS", delay, 1);
    VLink::Backend* a = VLink::CreateUdpBackend(42901, "127.0.0.1:42902", 0x0a000001);
    unsetenv("SMG_NET_DROP"); unsetenv("SMG_NET_DUP"); unsetenv("SMG_NET_DELAY_MS");
    VLink::Backend* b = VLink::CreateUdpBackend(42902, "127.0.0.1:42901", 0x0a000002);
    if (!a || !b || !a->local_vip(a->ctx) || !b->local_vip(b->ctx)) { fprintf(stderr, "backends did not open\n"); exit(2); }
    int got = 0, last = -1, flips = 0;
    char buf[256];
    VLink::PeerAddr from;
    for (int i = 0; i < 2000; ++i) {
        int v = i;
        a->send(a->ctx, nullptr, true, &v, sizeof v);
        if (i % 10 == 0) sleepMs(1);   // (paced: a delayed burst must not overflow B's socket buffer)
        int n;
        while ((n = b->recv(b->ctx, &from, buf, sizeof buf)) > 0) { ++got; int x; memcpy(&x, buf, 4); if (x < last) ++flips; last = x; }
    }
    for (int k = 0; k < 400; ++k) {   // let delayed ones out (A's next calls release them)
        a->recv(a->ctx, &from, buf, sizeof buf);
        int n;
        while ((n = b->recv(b->ctx, &from, buf, sizeof buf)) > 0) { ++got; int x; memcpy(&x, buf, 4); if (x < last) ++flips; last = x; }
        sleepMs(1);
    }
    a->close(a->ctx); b->close(b->ctx);
    VLink::DestroyUdpBackend(a); VLink::DestroyUdpBackend(b);
    if (outOrderFlips) *outOrderFlips = flips;
    return got;
}

int main() {
    int fails = 0;
    int got = run(nullptr, nullptr, nullptr, nullptr);
    printf("off: %d/2000\n", got);
    if (got != 2000) ++fails;
    got = run("20", nullptr, nullptr, nullptr);
    printf("drop 20%%: %d/2000\n", got);
    if (got < 1450 || got > 1750) ++fails;
    got = run(nullptr, "50", nullptr, nullptr);
    printf("dup 50%%: %d/2000\n", got);
    if (got < 2800 || got > 3200) ++fails;
    got = run(nullptr, nullptr, "100", nullptr);
    printf("delay 100 ms: %d/2000\n", got);
    if (got != 2000) ++fails;
    printf(fails ? "udp fault test FAILED\n" : "udp fault test passed\n");
    return fails ? 1 : 0;
}
