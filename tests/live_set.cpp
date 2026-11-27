// update25 host test (tools/test_live_set.sh): port/live_set.h, the table GXV_UPD_LIVESET answers port_live_actor_valid from, against the
// binary search over the sorted copy that it replaces. Random sets of heap-like (16-byte aligned, clustered) pointers, with repeats and a
// null; asked for every member, for pointers beside each member, for random ones and for null. Any difference is a failure. A negative
// control (PORT_LIVESET_NEG: the table built from all but one entry) must fail.
#include "port/live_set.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

struct Actor { int x; };

int main() {
    std::mt19937_64 rng(12345);
    long asks = 0, bad = 0, hits = 0;
    unsigned maxProbe = 0;
    for (int round = 0; round < 600; ++round) {
        const size_t n = round < 40 ? static_cast< size_t >(round) : static_cast< size_t >(rng() % 6000);
        std::vector< Actor* > raw;
        uintptr_t base = 0x80000000u + (rng() % 0x100000u) * 16;
        const bool clustered = (round & 1) != 0;
        for (size_t i = 0; i < n; ++i) {
            uintptr_t a = clustered ? base + (rng() % 40000) * 16 : 0x80000000u + (rng() % 0x3000000u) * 16;
            if (rng() % 97 == 0 && i != 0) a = reinterpret_cast< uintptr_t >(raw[rng() % raw.size()]);  // repeats
            raw.push_back(reinterpret_cast< Actor* >(a));
        }
        if (n > 3 && round % 5 == 0) raw[rng() % n] = nullptr;  // a null entry
        std::vector< Actor* > sorted = raw;
        std::sort(sorted.begin(), sorted.end());
        port::PtrSet< Actor > set;
        std::vector< Actor* > fed = sorted;
#ifdef PORT_LIVESET_NEG
        if (!fed.empty()) fed.pop_back();
#endif
        set.Build(fed.data(), fed.size());
        if (n == 0) {
            if (set.Built()) { printf("empty set built\n"); return 1; }
            continue;
        }
        auto ask = [&](Actor* p) {
            const bool want = std::binary_search(sorted.begin(), sorted.end(), p);
            const bool got = set.Built() ? set.Has(p) : false;
            ++asks;
            hits += want;
            if (want != got) {
                if (++bad <= 5) printf("MISMATCH round %d n %zu ptr %p: search %d table %d\n", round, n, static_cast< void* >(p), want, got);
            }
        };
        ask(nullptr);
        for (Actor* p : sorted) {
            ask(p);
            const uintptr_t a = reinterpret_cast< uintptr_t >(p);
            ask(reinterpret_cast< Actor* >(a + 8));
            ask(reinterpret_cast< Actor* >(a - 8));
            ask(reinterpret_cast< Actor* >(a + 16));
            ask(reinterpret_cast< Actor* >(a ^ 0x40000u));
        }
        for (int k = 0; k < 4000; ++k) ask(reinterpret_cast< Actor* >(0x80000000u + (rng() % 0x4000000u) * 4));
        // table load: at most half full
        size_t used = 0;
        for (Actor* q : set.slots) used += q != nullptr;
        if (used * 2 > set.slots.size()) { printf("table over half full: %zu of %zu\n", used, set.slots.size()); ++bad; }
        (void)maxProbe;
    }
    printf("asks %ld (members %ld), mismatches %ld\n", asks, hits, bad);
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
