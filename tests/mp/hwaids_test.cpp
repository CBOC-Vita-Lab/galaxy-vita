// hwaids: the pure logic of the UDP log mirror (platform/src/mp/mp_hwaids_logic.h): the target parser and the line ring.
#include "mp_hwaids_logic.h"

#include <stdio.h>
#include <string>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
    uint8_t ip[4];
    uint16_t port = 0;
    CHECK(mpaids::ParseTarget("192.168.1.20:9999", ip, &port) && ip[0] == 192 && ip[1] == 168 && ip[2] == 1 && ip[3] == 20 && port == 9999);
    CHECK(mpaids::ParseTarget("  10.0.0.1:1 \n", ip, &port) && ip[0] == 10 && port == 1);
    CHECK(mpaids::ParseTarget("255.255.255.255:65535", ip, &port) && port == 65535);
    const char* bad[] = {"", "1.2.3.4", "1.2.3:5", "1.2.3.4:", "1.2.3.4:0", "1.2.3.4:65536", "256.1.1.1:5", "1.2.3.4:5x", "a.b.c.d:5", "1.2.3.4.5:6",
                         "1..3.4:5", ":5", "1.2.3.4:123456", "1.2.3.4 :5", "-1.2.3.4:5", "1.2.3.4:5 6"};
    for (const char* b : bad) CHECK(!mpaids::ParseTarget(b, ip, &port));
    CHECK(!mpaids::ParseTarget(nullptr, ip, &port));

    // private targets only
    const uint8_t okIp[][4] = {{10, 1, 2, 3}, {127, 0, 0, 1}, {172, 16, 0, 5}, {172, 31, 9, 9}, {192, 168, 1, 20}};
    const uint8_t badIp[][4] = {{8, 8, 8, 8}, {172, 15, 0, 1}, {172, 32, 0, 1}, {192, 169, 1, 1}, {192, 168, 1, 255}, {255, 255, 255, 255}, {100, 64, 0, 1}, {169, 254, 1, 1}};
    for (const auto& a : okIp) CHECK(mpaids::IsPrivateTarget(a));
    for (const auto& a : badIp) CHECK(!mpaids::IsPrivateTarget(a));

    // ring: order, newline handling, batching
    static mpaids::LineRing<2048> r;
    r.Push("one\n", 4);
    r.Push("two", 3);
    r.Push("\n", 1);          // empty line: ignored
    r.Push("three\r\n", 7);
    char out[64];
    size_t n = r.PopBatch(out, sizeof out);
    CHECK(n == 14 && std::string(out, n) == "one\ntwo\nthree\n");
    CHECK(r.Empty() && r.TakeDropped() == 0);

    // a batch smaller than the content: whole lines only, the rest stays
    r.Push("aaaa", 4);
    r.Push("bbbb", 4);
    r.Push("cccc", 4);
    n = r.PopBatch(out, 11);
    CHECK(n == 10 && std::string(out, n) == "aaaa\nbbbb\n");
    n = r.PopBatch(out, 11);
    CHECK(n == 5 && std::string(out, n) == "cccc\n" && r.Empty());

    // full ring: the oldest lines are dropped and counted; the newest survive; wrap-around keeps the text intact
    std::string line(100, 'x');
    for (int i = 0; i < 100; i++) {
        char num[8];
        snprintf(num, sizeof num, "%03d", i);
        std::string l = std::string(num) + line;
        r.Push(l.data(), l.size());
    }
    CHECK(r.dropped > 0 && r.used <= 2048);
    unsigned total = r.dropped;
    std::string all;
    char big[1100];
    while (!r.Empty()) {
        n = r.PopBatch(big, sizeof big);
        CHECK(n > 0);
        all.append(big, n);
    }
    CHECK(all.size() > 0);
    CHECK(all.compare(all.size() - 104, 4, "099x") == 0);     // newest line is last
    size_t lines = 0;
    for (char c : all) lines += c == '\n';
    CHECK(lines + total == 100);
    int prev = -1;
    for (size_t at = 0; at < all.size(); at += 104) {          // every kept line whole and in order
        CHECK(all.size() >= at + 104 && all[at + 103] == '\n' && all[at + 102] == 'x' && all[at + 3] == 'x');
        const int v = atoi(all.substr(at, 3).c_str());
        CHECK(v > prev);
        prev = v;
    }
    CHECK(r.TakeDropped() == total && r.TakeDropped() == 0);

    // an over-long line is cut, not rejected
    std::string huge(5000, 'h');
    r.Push(huge.data(), huge.size());
    n = r.PopBatch(big, sizeof big);
    CHECK(n == 1001);

    // stress: random pushes and pops keep the ring consistent (used never exceeds N, content order stays)
    unsigned seed = 7;
    unsigned nextPush = 0, nextPop = 0;
    for (int i = 0; i < 20000; i++) {
        seed = seed * 1664525u + 1013904223u;
        if ((seed >> 24) % 3) {
            char l[40];
            const int k = snprintf(l, sizeof l, "L%u", nextPush++);
            r.Push(l, (size_t)k);
        } else {
            n = r.PopBatch(big, 50);
            size_t at = 0;
            while (at < n) {
                const size_t e = std::string(big, n).find('\n', at);
                const unsigned v = (unsigned)atoi(std::string(big + at + 1, e - at - 1).c_str());
                CHECK(v >= nextPop);
                nextPop = v + 1;
                at = e + 1;
            }
        }
        CHECK(r.used <= 2048);
    }
    if (fails == 0) printf("hwaids_test: all passed\n");
    return fails ? 1 : 0;
}
